/*
 * Copyright (c) 2013-2014 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "psmx2.h"

static inline void psmx2_am_enqueue_rma(struct psmx2_fid_domain *domain,
				        struct psmx2_am_request *req)
{
	fastlock_acquire(&domain->rma_queue.lock);
	slist_insert_tail(&req->list_entry, &domain->rma_queue.list);
	fastlock_release(&domain->rma_queue.lock);
}

static inline void psmx2_iov_copy(struct iovec *iov, size_t count,
				  size_t offset, const void *src,
				  size_t len)
{
	int i;
	size_t copy_len;

	for (i=0; i<count && len; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		}

		copy_len = iov[i].iov_len - offset;
		
		if (copy_len > len)
			copy_len = len;

		memcpy(iov[i].iov_base + offset, src, copy_len);

		src += copy_len;
		len -= copy_len;

		if (offset)
			offset = 0;
	}
}

/* RMA protocol:
 *
 * Write REQ:
 *	args[0].u32w0	cmd, src_vl, dst_vl, flag
 *	args[0].u32w1	len
 *	args[1].u64	req
 *	args[2].u64	addr
 *	args[3].u64	key
 *	args[4].u64	data (optional)
 *
 * Write REP:
 *	args[0].u32w0	cmd, flag
 *	args[0].u32w1	error
 *	args[1].u64	req
 *
 * Read REQ:
 *	args[0].u32w0	cmd, src_vl, dst_vl, flag
 *	args[0].u32w1	len
 *	args[1].u64	req
 *	args[2].u64	addr
 *	args[3].u64	key
 *	args[4].u64	offset / unused for long protocol
 *
 * Read REP:
 *	args[0].u32w0	cmd, flag
 *	args[0].u32w1	error
 *	args[1].u64	req
 *	args[2].u64	offset
 */

int psmx2_am_rma_handler(psm2_am_token_t token, psm2_amarg_t *args,
			 int nargs, void *src, uint32_t len)
{
	psm2_amarg_t rep_args[8];
	void *rma_addr;
	ssize_t rma_len;
	uint64_t key;
	int err = 0;
	int op_error = 0;
	int cmd, eom, has_data;
	struct psmx2_am_request *req;
	struct psmx2_cq_event *event;
	uint64_t offset;
	struct psmx2_fid_mr *mr;
	psm2_epaddr_t epaddr;
	uint8_t dst_vl, src_vl;
	struct psmx2_fid_domain *domain;
	struct psmx2_fid_ep *ep;

	psm2_am_get_source(token, &epaddr);

	cmd = PSMX2_AM_GET_OP(args[0].u32w0);
	dst_vl = PSMX2_AM_GET_DST(args[0].u32w0);

	domain = psmx2_active_fabric->active_domain;
	ep = domain->eps[dst_vl];

	eom = args[0].u32w0 & PSMX2_AM_EOM;
	has_data = args[0].u32w0 & PSMX2_AM_DATA;

	switch (cmd) {
	case PSMX2_AM_REQ_WRITE:
		rma_len = args[0].u32w1;
		rma_addr = (void *)(uintptr_t)args[2].u64;
		key = args[3].u64;
		mr = psmx2_mr_get(domain, key);
		op_error = mr ?
			psmx2_mr_validate(mr, (uint64_t)rma_addr, len, FI_REMOTE_WRITE) :
			-FI_EINVAL;
		if (!op_error) {
			rma_addr += mr->offset;
			memcpy(rma_addr, src, len);
			if (eom) {
				if (ep->recv_cq && has_data) {
					/* TODO: report the addr/len of the whole write */
					event = psmx2_cq_create_event(
							ep->recv_cq,
							0, /* context */
							rma_addr,
							FI_REMOTE_WRITE | FI_RMA | FI_REMOTE_CQ_DATA,
							rma_len,
							args[4].u64,
							0, /* tag */
							0, /* olen */
							0);

					if (event)
						psmx2_cq_enqueue_event(ep->recv_cq, event);
					else
						err = -FI_ENOMEM;
				}

				if (ep->remote_write_cntr)
					psmx2_cntr_inc(ep->remote_write_cntr);

				if (mr->cntr && mr->cntr != ep->remote_write_cntr)
					psmx2_cntr_inc(mr->cntr);
			}
		}
		if (eom || op_error) {
			rep_args[0].u32w0 = PSMX2_AM_REP_WRITE | eom;
			rep_args[0].u32w1 = op_error;
			rep_args[1].u64 = args[1].u64;
			err = psm2_am_reply_short(token, PSMX2_AM_RMA_HANDLER,
						  rep_args, 2, NULL, 0, 0,
						  NULL, NULL );
		}
		break;

	case PSMX2_AM_REQ_WRITE_LONG:
		src_vl = PSMX2_AM_GET_SRC(args[0].u32w0);
		rma_len = args[0].u32w1;
		rma_addr = (void *)(uintptr_t)args[2].u64;
		key = args[3].u64;
		mr = psmx2_mr_get(domain, key);
		op_error = mr ?
			psmx2_mr_validate(mr, (uint64_t)rma_addr, rma_len, FI_REMOTE_WRITE) :
			-FI_EINVAL;
		if (op_error) {
			rep_args[0].u32w0 = PSMX2_AM_REP_WRITE | eom;
			rep_args[0].u32w1 = op_error;
			rep_args[1].u64 = args[1].u64;
			err = psm2_am_reply_short(token, PSMX2_AM_RMA_HANDLER,
						  rep_args, 2, NULL, 0, 0,
						  NULL, NULL );
			break;
		}

		rma_addr += mr->offset;

		req = calloc(1, sizeof(*req));
		if (!req) {
			err = -FI_ENOMEM;
		} else {
			req->ep = ep;
			req->op = args[0].u32w0;
			req->write.addr = (uint64_t)rma_addr;
			req->write.len = rma_len;
			req->write.key = key;
			req->write.context = (void *)args[1].u64;
			req->write.peer_addr = (void *)epaddr;
			req->write.vl = dst_vl;
			req->write.peer_vl = src_vl;
			req->write.data = has_data ? args[4].u64 : 0;
			req->cq_flags = FI_REMOTE_WRITE | FI_RMA |
					(has_data ? FI_REMOTE_CQ_DATA : 0),
			PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_REMOTE_WRITE_CONTEXT;
			PSMX2_CTXT_USER(&req->fi_context) = mr;
			psmx2_am_enqueue_rma(mr->domain, req);
		}
		break;

	case PSMX2_AM_REQ_READ:
		rma_len = args[0].u32w1;
		rma_addr = (void *)(uintptr_t)args[2].u64;
		key = args[3].u64;
		offset = args[4].u64;
		mr = psmx2_mr_get(domain, key);
		op_error = mr ?
			psmx2_mr_validate(mr, (uint64_t)rma_addr, rma_len, FI_REMOTE_READ) :
			-FI_EINVAL;
		if (!op_error) {
			rma_addr += mr->offset;
		} else {
			rma_addr = NULL;
			rma_len = 0;
		}

		rep_args[0].u32w0 = PSMX2_AM_REP_READ | eom;
		rep_args[0].u32w1 = op_error;
		rep_args[1].u64 = args[1].u64;
		rep_args[2].u64 = offset;
		err = psm2_am_reply_short(token, PSMX2_AM_RMA_HANDLER,
				rep_args, 3, rma_addr, rma_len, 0,
				NULL, NULL );

		if (eom && !op_error) {
			if (ep->remote_read_cntr)
				psmx2_cntr_inc(ep->remote_read_cntr);
		}
		break;

	case PSMX2_AM_REQ_READ_LONG:
		src_vl = PSMX2_AM_GET_SRC(args[0].u32w0);
		rma_len = args[0].u32w1;
		rma_addr = (void *)(uintptr_t)args[2].u64;
		key = args[3].u64;
		mr = psmx2_mr_get(domain, key);
		op_error = mr ?
			psmx2_mr_validate(mr, (uint64_t)rma_addr, rma_len, FI_REMOTE_READ) :
			-FI_EINVAL;
		if (op_error) {
			rep_args[0].u32w0 = PSMX2_AM_REP_READ | eom;
			rep_args[0].u32w1 = op_error;
			rep_args[1].u64 = args[1].u64;
			rep_args[2].u64 = 0;
			err = psm2_am_reply_short(token, PSMX2_AM_RMA_HANDLER,
					rep_args, 3, NULL, 0, 0,
					NULL, NULL );
			break;
		}

		rma_addr += mr->offset;

		req = calloc(1, sizeof(*req));
		if (!req) {
			err = -FI_ENOMEM;
		} else {
			req->ep = ep;
			req->op = args[0].u32w0;
			req->read.addr = (uint64_t)rma_addr;
			req->read.len = rma_len;
			req->read.key = key;
			req->read.context = (void *)args[1].u64;
			req->read.peer_addr = (void *)epaddr;
			req->read.vl = dst_vl;
			req->read.peer_vl = src_vl;
			PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_REMOTE_READ_CONTEXT;
			PSMX2_CTXT_USER(&req->fi_context) = mr;
			psmx2_am_enqueue_rma(mr->domain, req);
		}
		break;

	case PSMX2_AM_REP_WRITE:
		req = (struct psmx2_am_request *)(uintptr_t)args[1].u64;
		assert(req->op == PSMX2_AM_REQ_WRITE);
		op_error = (int)args[0].u32w1;
		if (!req->error)
			req->error = op_error;
		if (eom) {
			if (req->ep->send_cq && !req->no_event) {
				event = psmx2_cq_create_event(
						req->ep->send_cq,
						req->write.context,
						req->write.buf,
						req->cq_flags,
						req->write.len,
						0, /* data */
						0, /* tag */
						0, /* olen */
						req->error);
				if (event)
					psmx2_cq_enqueue_event(req->ep->send_cq, event);
				else
					err = -FI_ENOMEM;
			}

			if (req->ep->write_cntr)
				psmx2_cntr_inc(req->ep->write_cntr);

			free(req);
		}
		break;

	case PSMX2_AM_REP_READ:
		req = (struct psmx2_am_request *)(uintptr_t)args[1].u64;
		assert(req->op == PSMX2_AM_REQ_READ || req->op == PSMX2_AM_REQ_READV);
		op_error = (int)args[0].u32w1;
		offset = args[2].u64;
		if (!req->error)
			req->error = op_error;
		if (!op_error) {
			if (req->op == PSMX2_AM_REQ_READ)
				memcpy(req->read.buf + offset, src, len);
			else 
				psmx2_iov_copy(req->iov, req->read.iov_count, offset, src, len);

			req->read.len_read += len;
		}
		if (eom || req->read.len == req->read.len_read) {
			if (!eom)
				FI_INFO(&psmx2_prov, FI_LOG_EP_DATA,
					"readv: short protocol finishes after long protocol.\n");
			if (req->ep->send_cq && !req->no_event) {
				event = psmx2_cq_create_event(
						req->ep->send_cq,
						req->read.context,
						req->read.buf,
						req->cq_flags,
						req->read.len_read,
						0, /* data */
						0, /* tag */
						req->read.len - req->read.len_read,
						req->error);
				if (event)
					psmx2_cq_enqueue_event(req->ep->send_cq, event);
				else
					err = -FI_ENOMEM;
			}

			if (req->ep->read_cntr)
				psmx2_cntr_inc(req->ep->read_cntr);

			free(req);
		}
		break;

	default:
		err = -FI_EINVAL;
	}
	return err;
}

static ssize_t psmx2_rma_self(int am_cmd,
			      struct psmx2_fid_ep *ep,
			      struct psmx2_fid_ep *dst_ep,
			      void *buf, size_t len, void *desc,
			      uint64_t addr, uint64_t key,
			      void *context, uint64_t flags, uint64_t data)
{
	struct psmx2_fid_mr *mr;
	struct psmx2_cq_event *event;
	struct psmx2_fid_cntr *cntr = NULL;
	struct psmx2_fid_cntr *mr_cntr = NULL;
	struct psmx2_fid_cq *cq = NULL;
	int no_event;
	int err = 0;
	int op_error = 0;
	int access;
	void *dst, *src;
	uint64_t cq_flags;
	struct iovec *iov = buf;
	size_t iov_count = len;
	int i;

	switch (am_cmd) {
	case PSMX2_AM_REQ_WRITE:
		access = FI_REMOTE_WRITE;
		cq_flags = FI_WRITE | FI_RMA;
		break;
	case PSMX2_AM_REQ_WRITEV:
		access = FI_REMOTE_WRITE;
		cq_flags = FI_WRITE | FI_RMA;
		len = 0;
		for (i=0; i<iov_count; i++)
			len += iov[i].iov_len;
		break;
	case PSMX2_AM_REQ_READ:
		access = FI_REMOTE_READ;
		cq_flags = FI_READ | FI_RMA;
		break;
	case PSMX2_AM_REQ_READV:
		access = FI_REMOTE_READ;
		cq_flags = FI_READ | FI_RMA;
		len = 0;
		for (i=0; i<iov_count; i++)
			len += iov[i].iov_len;
	default:
		return -FI_EINVAL;
	}

	mr = psmx2_mr_get(psmx2_active_fabric->active_domain, key);
	op_error = mr ? psmx2_mr_validate(mr, addr, len, access) : -FI_EINVAL;

	if (!op_error) {
		addr += mr->offset;
		switch (am_cmd) {
		case PSMX2_AM_REQ_WRITE:
			cntr = dst_ep->remote_write_cntr;
			if (flags & FI_REMOTE_CQ_DATA)
				cq = dst_ep->recv_cq;
			if (mr->cntr != cntr)
				mr_cntr = mr->cntr;
			memcpy((void *)addr, buf, len);
			break;

		case PSMX2_AM_REQ_WRITEV:
			cntr = dst_ep->remote_write_cntr;
			if (flags & FI_REMOTE_CQ_DATA)
				cq = dst_ep->recv_cq;
			if (mr->cntr != cntr)
				mr_cntr = mr->cntr;
			dst = (void *)addr;
			for (i=0; i<iov_count; i++)
				if (iov[i].iov_len) {
					memcpy(dst, iov[i].iov_base, iov[i].iov_len);
					dst += iov[i].iov_len;
				}
			break;

		case PSMX2_AM_REQ_READ:
			cntr = dst_ep->remote_read_cntr;
			memcpy(buf, (void *)addr, len);
			break;

		case PSMX2_AM_REQ_READV:
			cntr = dst_ep->remote_read_cntr;
			src = (void *)addr;
			for (i=0; i<iov_count; i++)
				if (iov[i].iov_len) {
					memcpy(iov[i].iov_base, src, iov[i].iov_len);
					src += iov[i].iov_len;
				}
			break;
		}

		if (cq) {
			event = psmx2_cq_create_event(
					cq,
					0, /* context */
					(void *)addr,
					FI_REMOTE_WRITE | FI_RMA | FI_REMOTE_CQ_DATA,
					len,
					data,
					0, /* tag */
					0, /* olen */
					0 /* err */);

			if (event)
				psmx2_cq_enqueue_event(cq, event);
			else
				err = -FI_ENOMEM;
		}

		if (cntr)
			psmx2_cntr_inc(cntr);

		if (mr_cntr)
			psmx2_cntr_inc(mr_cntr);
	}

	no_event = (flags & PSMX2_NO_COMPLETION) ||
		   (ep->send_selective_completion && !(flags & FI_COMPLETION));

	if (ep->send_cq && !no_event) {
		event = psmx2_cq_create_event(
				ep->send_cq,
				context,
				(void *)buf,
				cq_flags,
				len,
				0, /* data */
				0, /* tag */
				0, /* olen */
				op_error);
		if (event)
			psmx2_cq_enqueue_event(ep->send_cq, event);
		else
			err = -FI_ENOMEM;
	}

	switch (am_cmd) {
	case PSMX2_AM_REQ_WRITE:
	case PSMX2_AM_REQ_WRITEV:
		if (ep->write_cntr)
			psmx2_cntr_inc(ep->write_cntr);
		break;

	case PSMX2_AM_REQ_READ:
	case PSMX2_AM_REQ_READV:
		if (ep->read_cntr)
			psmx2_cntr_inc(ep->read_cntr);
		break;
	}

	return err;
}

void psmx2_am_ack_rma(struct psmx2_am_request *req)
{
	psm2_amarg_t args[8];

	if ((req->op & PSMX2_AM_OP_MASK) != PSMX2_AM_REQ_WRITE_LONG)
		return;

	args[0].u32w0 = PSMX2_AM_REP_WRITE | PSMX2_AM_EOM;
	args[0].u32w1 = req->error;
	args[1].u64 = (uint64_t)(uintptr_t)req->write.context;

	psm2_am_request_short(req->write.peer_addr,
			      PSMX2_AM_RMA_HANDLER, args, 2, NULL, 0,
			      PSM2_AM_FLAG_NOREPLY, NULL, NULL);
}

int psmx2_am_process_rma(struct psmx2_fid_domain *domain,
			 struct psmx2_am_request *req)
{
	int err;
	psm2_mq_req_t psm2_req;
	psm2_mq_tag_t psm2_tag, psm2_tagsel;
	uint32_t tag32;

	if ((req->op & PSMX2_AM_OP_MASK) == PSMX2_AM_REQ_WRITE_LONG) {
		tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, req->write.peer_vl, req->write.vl);
		PSMX2_SET_TAG(psm2_tag, (uint64_t)req->write.context, tag32);
		PSMX2_SET_TAG(psm2_tagsel, -1ULL, -1);
		err = psm2_mq_irecv2(domain->psm2_mq,
				     (psm2_epaddr_t)req->write.peer_addr,
				     &psm2_tag, &psm2_tagsel, 0,
				     (void *)req->write.addr, req->write.len,
				     (void *)&req->fi_context, &psm2_req);
	} else {
		tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, req->read.vl, req->read.peer_vl);
		PSMX2_SET_TAG(psm2_tag, (uint64_t)req->read.context, tag32);
		err = psm2_mq_isend2(domain->psm2_mq,
				     (psm2_epaddr_t)req->read.peer_addr,
				     0, &psm2_tag,
				     (void *)req->read.addr, req->read.len,
				     (void *)&req->fi_context, &psm2_req);
	}

	return psmx2_errno(err);
}

ssize_t psmx2_read_generic(struct fid_ep *ep, void *buf, size_t len,
			   void *desc, fi_addr_t src_addr,
			   uint64_t addr, uint64_t key, void *context,
			   uint64_t flags)
{
	struct psmx2_fid_ep *ep_priv;
	struct psmx2_fid_av *av;
	struct psmx2_epaddr_context *epaddr_context;
	struct psmx2_am_request *req;
	psm2_amarg_t args[8];
	int chunk_size;
	size_t offset = 0;
	psm2_epaddr_t psm2_epaddr;
	uint8_t vlane;
	psm2_mq_req_t psm2_req;
	psm2_mq_tag_t psm2_tag, psm2_tagsel;
	uint32_t tag32;
	size_t idx;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (flags & FI_TRIGGER) {
		struct psmx2_trigger *trigger;
		struct fi_triggered_context *ctxt = context;

		trigger = calloc(1, sizeof(*trigger));
		if (!trigger)
			return -FI_ENOMEM;

		trigger->op = PSMX2_TRIGGERED_READ;
		trigger->cntr = container_of(ctxt->trigger.threshold.cntr,
					     struct psmx2_fid_cntr, cntr);
		trigger->threshold = ctxt->trigger.threshold.threshold;
		trigger->read.ep = ep;
		trigger->read.buf = buf;
		trigger->read.len = len;
		trigger->read.desc = desc;
		trigger->read.src_addr = src_addr;
		trigger->read.addr = addr;
		trigger->read.key = key;
		trigger->read.context = context;
		trigger->read.flags = flags & ~FI_TRIGGER;

		psmx2_cntr_add_trigger(trigger->cntr, trigger);
		return 0;
	}

	if (!buf)
		return -FI_EINVAL;

	av = ep_priv->av;
	if (av && av->type == FI_AV_TABLE) {
		idx = src_addr;
		if (idx >= av->last)
			return -FI_EINVAL;

		psm2_epaddr = av->epaddrs[idx];
		vlane = av->vlanes[idx];
	} else {
		if (!src_addr)
			return -FI_EINVAL;

		psm2_epaddr = PSMX2_ADDR_TO_EP(src_addr);
		vlane = PSMX2_ADDR_TO_VL(src_addr);
	}

	epaddr_context = psm2_epaddr_getctxt((void *)psm2_epaddr);
	if (epaddr_context->epid == ep_priv->domain->psm2_epid)
		return psmx2_rma_self(PSMX2_AM_REQ_READ, ep_priv,
				      ep_priv->domain->eps[vlane],
				      buf, len, desc, addr, key,
				      context, flags, 0);

	req = calloc(1, sizeof(*req));
	if (!req)
		return -FI_ENOMEM;

	req->op = PSMX2_AM_REQ_READ;
	req->read.buf = buf;
	req->read.len = len;
	req->read.addr = addr;	/* needed? */
	req->read.key = key; 	/* needed? */
	req->read.context = context;
	req->ep = ep_priv;
	req->cq_flags = FI_READ | FI_RMA;
	PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_READ_CONTEXT;
	PSMX2_CTXT_USER(&req->fi_context) = context;
	PSMX2_CTXT_EP(&req->fi_context) = ep_priv;

	if (ep_priv->send_selective_completion && !(flags & FI_COMPLETION)) {
		PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_NOCOMP_READ_CONTEXT;
		req->no_event = 1;
	}

	chunk_size = psmx2_am_param.max_reply_short;

	args[0].u32w0 = 0;
	PSMX2_AM_SET_SRC(args[0].u32w0, ep_priv->vlane);
	PSMX2_AM_SET_DST(args[0].u32w0, vlane);

	if (psmx2_env.tagged_rma && len > chunk_size) {
		tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, vlane, ep_priv->vlane);
		PSMX2_SET_TAG(psm2_tag, (uint64_t)req, tag32);
		PSMX2_SET_TAG(psm2_tagsel, -1ULL, -1);
		psm2_mq_irecv2(ep_priv->domain->psm2_mq, psm2_epaddr,
			       &psm2_tag, &psm2_tagsel, 0, buf, len,
			       (void *)&req->fi_context, &psm2_req);

		PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_READ_LONG);
		args[0].u32w1 = len;
		args[1].u64 = (uint64_t)req;
		args[2].u64 = addr;
		args[3].u64 = key;
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
				      args, 4, NULL, 0, 0, NULL, NULL);

		return 0;
	}

	PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_READ);
	args[1].u64 = (uint64_t)(uintptr_t)req;
	args[3].u64 = key;
	while (len > chunk_size) {
		args[0].u32w1 = chunk_size;
		args[2].u64 = addr;
		args[4].u64 = offset;
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
				      args, 5, NULL, 0, 0, NULL, NULL);
		addr += chunk_size;
		len -= chunk_size;
		offset += chunk_size;
	}

	PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_EOM);
	args[0].u32w1 = len;
	args[2].u64 = addr;
	args[4].u64 = offset;
	psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
			      args, 5, NULL, 0, 0, NULL, NULL);

	return 0;
}

ssize_t psmx2_readv_generic(struct fid_ep *ep, const struct iovec *iov,
			    void *desc, size_t count, fi_addr_t src_addr,
			    uint64_t addr, uint64_t key, void *context,
			    uint64_t flags)
{
	struct psmx2_fid_ep *ep_priv;
	struct psmx2_fid_av *av;
	struct psmx2_epaddr_context *epaddr_context;
	struct psmx2_am_request *req;
	psm2_amarg_t args[8];
	int chunk_size;
	size_t offset = 0;
	psm2_epaddr_t psm2_epaddr;
	uint8_t vlane;
	psm2_mq_req_t psm2_req;
	psm2_mq_tag_t psm2_tag, psm2_tagsel;
	uint32_t tag32;
	size_t idx;
	size_t total_len, long_len, short_len;
	void *long_buf;
	int i;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (flags & FI_TRIGGER) {
		struct psmx2_trigger *trigger;
		struct fi_triggered_context *ctxt = context;

		trigger = calloc(1, sizeof(*trigger));
		if (!trigger)
			return -FI_ENOMEM;

		trigger->op = PSMX2_TRIGGERED_READV;
		trigger->cntr = container_of(ctxt->trigger.threshold.cntr,
					     struct psmx2_fid_cntr, cntr);
		trigger->threshold = ctxt->trigger.threshold.threshold;
		trigger->readv.ep = ep;
		trigger->readv.iov = iov;
		trigger->readv.count = count;
		trigger->readv.desc = desc;
		trigger->readv.src_addr = src_addr;
		trigger->readv.addr = addr;
		trigger->readv.key = key;
		trigger->readv.context = context;
		trigger->readv.flags = flags & ~FI_TRIGGER;

		psmx2_cntr_add_trigger(trigger->cntr, trigger);
		return 0;
	}

	av = ep_priv->av;
	if (av && av->type == FI_AV_TABLE) {
		idx = src_addr;
		if (idx >= av->last)
			return -FI_EINVAL;

		psm2_epaddr = av->epaddrs[idx];
		vlane = av->vlanes[idx];
	} else {
		if (!src_addr)
			return -FI_EINVAL;

		psm2_epaddr = PSMX2_ADDR_TO_EP(src_addr);
		vlane = PSMX2_ADDR_TO_VL(src_addr);
	}

	epaddr_context = psm2_epaddr_getctxt((void *)psm2_epaddr);
	if (epaddr_context->epid == ep_priv->domain->psm2_epid)
		return psmx2_rma_self(PSMX2_AM_REQ_READV, ep_priv,
				      ep_priv->domain->eps[vlane],
				      (void *)iov, count, desc, addr,
				      key, context, flags, 0);

	total_len = 0;
	for (i=0; i<count; i++)
		total_len += iov[i].iov_len;

	req = calloc(1, sizeof(*req) + count * sizeof(struct iovec));
	if (!req)
		return -FI_ENOMEM;

	memcpy(req->iov, iov, count * sizeof(struct iovec));

	req->op = PSMX2_AM_REQ_READV;
	req->read.iov_count = count;
	req->read.len = total_len;
	req->read.addr = addr;	/* needed? */
	req->read.key = key; 	/* needed? */
	req->read.context = context;
	req->ep = ep_priv;
	req->cq_flags = FI_READ | FI_RMA;
	PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_READ_CONTEXT;
	PSMX2_CTXT_USER(&req->fi_context) = context;
	PSMX2_CTXT_EP(&req->fi_context) = ep_priv;

	if (ep_priv->send_selective_completion && !(flags & FI_COMPLETION)) {
		PSMX2_CTXT_TYPE(&req->fi_context) = PSMX2_NOCOMP_READ_CONTEXT;
		req->no_event = 1;
	}

	chunk_size = psmx2_am_param.max_reply_short;

	long_len = 0;
	if (psmx2_env.tagged_rma) {
		for (i=count-1; i>=0; i--) {
			if (iov[i].iov_len > chunk_size) {
				long_buf = iov[i].iov_base;
				long_len = iov[i].iov_len;
				break;
			} else if (iov[i].iov_len) {
				break;
			}
		}
	}

	short_len = total_len - long_len;

	/* Use short protocol for all but the last segment (long_len) */
	args[0].u32w0 = 0;
	PSMX2_AM_SET_SRC(args[0].u32w0, ep_priv->vlane);
	PSMX2_AM_SET_DST(args[0].u32w0, vlane);
	PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_READ);
	args[1].u64 = (uint64_t)(uintptr_t)req;
	args[3].u64 = key;
	while (short_len > chunk_size) {
		args[0].u32w1 = chunk_size;
		args[2].u64 = addr;
		args[4].u64 = offset;
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
				      args, 5, NULL, 0, 0, NULL, NULL);
		addr += chunk_size;
		short_len -= chunk_size;
		offset += chunk_size;
	}

	if (!long_len)
		PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_EOM);
	args[0].u32w1 = short_len;
	args[2].u64 = addr;
	args[4].u64 = offset;
	psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
			      args, 5, NULL, 0, 0, NULL, NULL);

	/* Use the long protocol for the last segment */
	if (long_len) {
		tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, vlane, ep_priv->vlane);
		PSMX2_SET_TAG(psm2_tag, (uint64_t)req, tag32);
		PSMX2_SET_TAG(psm2_tagsel, -1ULL, -1);
		psm2_mq_irecv2(ep_priv->domain->psm2_mq, psm2_epaddr,
			       &psm2_tag, &psm2_tagsel, 0,
			       long_buf, long_len,
			       (void *)&req->fi_context, &psm2_req);

		PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_READ_LONG);
		args[0].u32w1 = long_len;
		args[1].u64 = (uint64_t)req;
		args[2].u64 = addr + short_len;
		args[3].u64 = key;
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER,
				      args, 4, NULL, 0, 0, NULL, NULL);
	}

	return 0;
}

static ssize_t psmx2_read(struct fid_ep *ep, void *buf, size_t len,
			  void *desc, fi_addr_t src_addr,
			  uint64_t addr, uint64_t key, void *context)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	return psmx2_read_generic(ep, buf, len, desc, src_addr, addr,
				  key, context, ep_priv->tx_flags);
}

static ssize_t psmx2_readmsg(struct fid_ep *ep,
			     const struct fi_msg_rma *msg,
			     uint64_t flags)
{
	if (!msg || !msg->iov_count || !msg->msg_iov ||
	    !msg->rma_iov || msg->rma_iov_count != 1)
		return -FI_EINVAL;

	if (msg->iov_count > 1)
		return psmx2_readv_generic(ep, msg->msg_iov,
					   msg->desc ? msg->desc[0] : NULL,
					   msg->iov_count, msg->addr,
					   msg->rma_iov[0].addr,
					   msg->rma_iov[0].key,
					   msg->context, flags);

	return psmx2_read_generic(ep, msg->msg_iov[0].iov_base,
				  msg->msg_iov[0].iov_len,
				  msg->desc ? msg->desc[0] : NULL,
				  msg->addr, msg->rma_iov[0].addr,
				  msg->rma_iov[0].key, msg->context,
				  flags);
}

static ssize_t psmx2_readv(struct fid_ep *ep, const struct iovec *iov,
		           void **desc, size_t count, fi_addr_t src_addr,
		           uint64_t addr, uint64_t key, void *context)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (!iov || !count)
		return -FI_EINVAL;

	if (count > 1)
		return psmx2_readv_generic(ep, iov, desc ? desc[0] : NULL,
					   count, src_addr, addr, key,
					   context, ep_priv->tx_flags);

	return psmx2_read(ep, iov->iov_base, iov->iov_len,
			  desc ? desc[0] : NULL, src_addr, addr, key, context);
}

ssize_t psmx2_write_generic(struct fid_ep *ep, const void *buf, size_t len,
			    void *desc, fi_addr_t dest_addr,
			    uint64_t addr, uint64_t key, void *context,
			    uint64_t flags, uint64_t data)
{
	struct psmx2_fid_ep *ep_priv;
	struct psmx2_fid_av *av;
	struct psmx2_epaddr_context *epaddr_context;
	struct psmx2_am_request *req;
	psm2_amarg_t args[8];
	int nargs;
	int am_flags = PSM2_AM_FLAG_ASYNC;
	int chunk_size;
	psm2_epaddr_t psm2_epaddr;
	uint8_t vlane;
	psm2_mq_req_t psm2_req;
	psm2_mq_tag_t psm2_tag;
	uint32_t tag32;
	size_t idx;
	void *psm2_context;
	int no_event;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (flags & FI_TRIGGER) {
		struct psmx2_trigger *trigger;
		struct fi_triggered_context *ctxt = context;

		trigger = calloc(1, sizeof(*trigger));
		if (!trigger)
			return -FI_ENOMEM;

		trigger->op = PSMX2_TRIGGERED_WRITE;
		trigger->cntr = container_of(ctxt->trigger.threshold.cntr,
					     struct psmx2_fid_cntr, cntr);
		trigger->threshold = ctxt->trigger.threshold.threshold;
		trigger->write.ep = ep;
		trigger->write.buf = buf;
		trigger->write.len = len;
		trigger->write.desc = desc;
		trigger->write.dest_addr = dest_addr;
		trigger->write.addr = addr;
		trigger->write.key = key;
		trigger->write.context = context;
		trigger->write.flags = flags & ~FI_TRIGGER;
		trigger->write.data = data;

		psmx2_cntr_add_trigger(trigger->cntr, trigger);
		return 0;
	}

	if (!buf)
		return -FI_EINVAL;

	av = ep_priv->av;
	if (av && av->type == FI_AV_TABLE) {
		idx = dest_addr;
		if (idx >= av->last)
			return -FI_EINVAL;

		psm2_epaddr = av->epaddrs[idx];
		vlane = av->vlanes[idx];
	} else {
		if (!dest_addr)
			return -FI_EINVAL;

		psm2_epaddr = PSMX2_ADDR_TO_EP(dest_addr);
		vlane = PSMX2_ADDR_TO_VL(dest_addr);
	}

	epaddr_context = psm2_epaddr_getctxt((void *)psm2_epaddr);
	if (epaddr_context->epid == ep_priv->domain->psm2_epid)
		return psmx2_rma_self(PSMX2_AM_REQ_WRITE, ep_priv,
				      ep_priv->domain->eps[vlane],
				      (void *)buf, len, desc, addr,
				      key, context, flags, data);

	no_event = (flags & PSMX2_NO_COMPLETION) ||
		   (ep_priv->send_selective_completion && !(flags & FI_COMPLETION));

	if (flags & FI_INJECT) {
		if (len > PSMX2_INJECT_SIZE)
			return -FI_EMSGSIZE;

		req = malloc(sizeof(*req) + len);
		if (!req)
			return -FI_ENOMEM;

		memset((void *)req, 0, sizeof(*req));
		memcpy((void *)req + sizeof(*req), (void *)buf, len);
		buf = (void *)req + sizeof(*req);
	} else {
		req = calloc(1, sizeof(*req));
		if (!req)
			return -FI_ENOMEM;

		PSMX2_CTXT_TYPE(&req->fi_context) = no_event ?
						    PSMX2_NOCOMP_WRITE_CONTEXT :
						    PSMX2_WRITE_CONTEXT;
	}

	req->no_event = no_event;
	req->op = PSMX2_AM_REQ_WRITE;
	req->write.buf = (void *)buf;
	req->write.len = len;
	req->write.addr = addr;	/* needed? */
	req->write.key = key; 	/* needed? */
	req->write.context = context;
	req->ep = ep_priv;
	req->cq_flags = FI_WRITE | FI_RMA;
	PSMX2_CTXT_USER(&req->fi_context) = context;
	PSMX2_CTXT_EP(&req->fi_context) = ep_priv;

	chunk_size = psmx2_am_param.max_request_short;

	args[0].u32w0 = 0;
	PSMX2_AM_SET_SRC(args[0].u32w0, ep_priv->vlane);
	PSMX2_AM_SET_DST(args[0].u32w0, vlane);

	if (psmx2_env.tagged_rma && len > chunk_size) {
		tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, ep_priv->vlane, vlane);
		PSMX2_SET_TAG(psm2_tag, (uint64_t)req, tag32);
		PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_WRITE_LONG);
		args[0].u32w1 = len;
		args[1].u64 = (uint64_t)req;
		args[2].u64 = addr;
		args[3].u64 = key;
		nargs = 4;
		if (flags & FI_REMOTE_CQ_DATA) {
			PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_DATA);
			args[4].u64 = data;
			nargs++;
		}

		if (flags & FI_DELIVERY_COMPLETE) {
			args[0].u32w0 |= PSMX2_AM_FORCE_ACK;
			psm2_context = NULL;
		} else {
			psm2_context = (void *)&req->fi_context;
		}

		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args,
				      nargs, NULL, 0, am_flags, NULL, NULL);

		psm2_mq_isend2(ep_priv->domain->psm2_mq, psm2_epaddr, 0,
			       &psm2_tag, buf, len, psm2_context, &psm2_req);

		return 0;
	}

	PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_WRITE);
	nargs = 4;
	while (len > chunk_size) {
		args[0].u32w1 = chunk_size;
		args[1].u64 = (uint64_t)(uintptr_t)req;
		args[2].u64 = addr;
		args[3].u64 = key;
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args,
				      nargs, (void *)buf, chunk_size, am_flags,
				      NULL, NULL);
		buf += chunk_size;
		addr += chunk_size;
		len -= chunk_size;
	}

	args[0].u32w1 = len;
	args[1].u64 = (uint64_t)(uintptr_t)req;
	args[2].u64 = addr;
	args[3].u64 = key;
	if (flags & FI_REMOTE_CQ_DATA) {
		PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_DATA | PSMX2_AM_EOM);
		args[4].u64 = data;
		nargs++;
	} else {
		PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_EOM);
	}
	psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args, nargs,
			      (void *)buf, len, am_flags, NULL, NULL);

	return 0;
}

ssize_t psmx2_writev_generic(struct fid_ep *ep, const struct iovec *iov,
		             void **desc, size_t count, fi_addr_t dest_addr,
		             uint64_t addr, uint64_t key, void *context,
			     uint64_t flags, uint64_t data)
{
	struct psmx2_fid_ep *ep_priv;
	struct psmx2_fid_av *av;
	struct psmx2_epaddr_context *epaddr_context;
	struct psmx2_am_request *req;
	psm2_amarg_t args[8];
	int nargs;
	int am_flags = PSM2_AM_FLAG_ASYNC;
	int chunk_size;
	psm2_epaddr_t psm2_epaddr;
	uint8_t vlane;
	psm2_mq_req_t psm2_req;
	psm2_mq_tag_t psm2_tag;
	uint32_t tag32;
	size_t idx;
	void *psm2_context;
	int no_event;
	size_t total_len, len, len_sent;
	void *buf, *p;
	int i;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (flags & FI_TRIGGER) {
		struct psmx2_trigger *trigger;
		struct fi_triggered_context *ctxt = context;

		trigger = calloc(1, sizeof(*trigger));
		if (!trigger)
			return -FI_ENOMEM;

		trigger->op = PSMX2_TRIGGERED_WRITEV;
		trigger->cntr = container_of(ctxt->trigger.threshold.cntr,
					     struct psmx2_fid_cntr, cntr);
		trigger->threshold = ctxt->trigger.threshold.threshold;
		trigger->writev.ep = ep;
		trigger->writev.iov = iov;
		trigger->writev.count = count;
		trigger->writev.desc = desc;
		trigger->writev.dest_addr = dest_addr;
		trigger->writev.addr = addr;
		trigger->writev.key = key;
		trigger->writev.context = context;
		trigger->writev.flags = flags & ~FI_TRIGGER;
		trigger->writev.data = data;

		psmx2_cntr_add_trigger(trigger->cntr, trigger);
		return 0;
	}

	av = ep_priv->av;
	if (av && av->type == FI_AV_TABLE) {
		idx = dest_addr;
		if (idx >= av->last)
			return -FI_EINVAL;

		psm2_epaddr = av->epaddrs[idx];
		vlane = av->vlanes[idx];
	} else {
		if (!dest_addr)
			return -FI_EINVAL;

		psm2_epaddr = PSMX2_ADDR_TO_EP(dest_addr);
		vlane = PSMX2_ADDR_TO_VL(dest_addr);
	}

	epaddr_context = psm2_epaddr_getctxt((void *)psm2_epaddr);
	if (epaddr_context->epid == ep_priv->domain->psm2_epid)
		return psmx2_rma_self(PSMX2_AM_REQ_WRITEV, ep_priv,
				      ep_priv->domain->eps[vlane],
				      (void *)iov, count, desc, addr,
				      key, context, flags, data);

	no_event = (flags & PSMX2_NO_COMPLETION) ||
		   (ep_priv->send_selective_completion && !(flags & FI_COMPLETION));

	total_len = 0;
	for (i=0; i<count; i++)
		total_len += iov[i].iov_len;

	chunk_size = psmx2_am_param.max_request_short;

	/* Case 1: fit into a AM message, then pack and send */
	if (total_len <= chunk_size) {
		req = malloc(sizeof(*req) + total_len);
		if (!req)
			return -FI_ENOMEM;

		memset((void *)req, 0, sizeof(*req));
		p = (void *)req + sizeof(*req);
		for (i=0; i<count; i++) {
			if (iov[i].iov_len) {
				memcpy(p, iov[i].iov_base, iov[i].iov_len);
				p += iov[i].iov_len;
			}
		}
		buf = (void *)req + sizeof(*req);
		len = total_len;

		req->no_event = no_event;
		req->op = PSMX2_AM_REQ_WRITE;
		req->write.buf = (void *)buf;
		req->write.len = len;
		req->write.addr = addr;	/* needed? */
		req->write.key = key; 	/* needed? */
		req->write.context = context;
		req->ep = ep_priv;
		req->cq_flags = FI_WRITE | FI_RMA;
		PSMX2_CTXT_USER(&req->fi_context) = context;
		PSMX2_CTXT_EP(&req->fi_context) = ep_priv;

		args[0].u32w0 = 0;
		PSMX2_AM_SET_SRC(args[0].u32w0, ep_priv->vlane);
		PSMX2_AM_SET_DST(args[0].u32w0, vlane);
		PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_WRITE);
		args[0].u32w1 = len;
		args[1].u64 = (uint64_t)(uintptr_t)req;
		args[2].u64 = addr;
		args[3].u64 = key;
		nargs = 4;
		if (flags & FI_REMOTE_CQ_DATA) {
			PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_DATA | PSMX2_AM_EOM);
			args[4].u64 = data;
			nargs++;
		} else {
			PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_EOM);
		}
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args, nargs,
				      (void *)buf, len, am_flags, NULL, NULL);

		return 0;
	}

	if (flags & FI_INJECT)
		return -FI_EMSGSIZE;

	req = calloc(1, sizeof(*req));
	if (!req)
		return -FI_ENOMEM;

	PSMX2_CTXT_TYPE(&req->fi_context) = no_event ?
					    PSMX2_NOCOMP_WRITE_CONTEXT :
					    PSMX2_WRITE_CONTEXT;

	req->no_event = no_event;
	req->op = PSMX2_AM_REQ_WRITE;
	req->write.buf = (void *)iov[0].iov_base;
	req->write.len = total_len;
	req->write.addr = addr;	/* needed? */
	req->write.key = key; 	/* needed? */
	req->write.context = context;
	req->ep = ep_priv;
	req->cq_flags = FI_WRITE | FI_RMA;
	PSMX2_CTXT_USER(&req->fi_context) = context;
	PSMX2_CTXT_EP(&req->fi_context) = ep_priv;

	/* Case 2: send iov in sequence */
	args[0].u32w0 = 0;
	PSMX2_AM_SET_SRC(args[0].u32w0, ep_priv->vlane);
	PSMX2_AM_SET_DST(args[0].u32w0, vlane);

	len_sent = 0;
	for (i=0; i<count; i++) {
		if (!iov[i].iov_len)
			continue;

		/* Case 2.1: use long protocol for the last segment if it is large */
		if (psmx2_env.tagged_rma && iov[i].iov_len > chunk_size &&
		    len_sent + iov[i].iov_len == total_len) {
			tag32 = PSMX2_TAG32(PSMX2_RMA_BIT, ep_priv->vlane, vlane);
			PSMX2_SET_TAG(psm2_tag, (uint64_t)req, tag32);
			PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_WRITE_LONG);
			args[0].u32w1 = iov[i].iov_len;
			args[1].u64 = (uint64_t)req;
			args[2].u64 = addr;
			args[3].u64 = key;
			nargs = 4;
			if (flags & FI_REMOTE_CQ_DATA) {
				PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_DATA);
				args[4].u64 = data;
				nargs++;
			}

			if (flags & FI_DELIVERY_COMPLETE) {
				args[0].u32w0 |= PSMX2_AM_FORCE_ACK;
				psm2_context = NULL;
			} else {
				psm2_context = (void *)&req->fi_context;
			}

			psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args,
					      nargs, NULL, 0, am_flags, NULL, NULL);

			psm2_mq_isend2(ep_priv->domain->psm2_mq, psm2_epaddr, 0,
				       &psm2_tag, iov[i].iov_base, iov[i].iov_len,
				       psm2_context, &psm2_req);

			return 0;
		}

		/* Case 2.2: use short protocol all other segments */
		PSMX2_AM_SET_OP(args[0].u32w0, PSMX2_AM_REQ_WRITE);
		nargs = 4;
		buf = iov[i].iov_base;
		len = iov[i].iov_len;
		while (len > chunk_size) {
			args[0].u32w1 = chunk_size;
			args[1].u64 = (uint64_t)(uintptr_t)req;
			args[2].u64 = addr;
			args[3].u64 = key;
			psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args,
					      nargs, (void *)buf, chunk_size, am_flags,
					      NULL, NULL);
			buf += chunk_size;
			addr += chunk_size;
			len -= chunk_size;
			len_sent += chunk_size;
		}

		args[0].u32w1 = len;
		args[1].u64 = (uint64_t)(uintptr_t)req;
		args[2].u64 = addr;
		args[3].u64 = key;
		if (len_sent + len == total_len) {
			if (flags & FI_REMOTE_CQ_DATA) {
				PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_DATA | PSMX2_AM_EOM);
				args[4].u64 = data;
				nargs++;
			} else {
				PSMX2_AM_SET_FLAG(args[0].u32w0, PSMX2_AM_EOM);
			}
		}
		psm2_am_request_short(psm2_epaddr, PSMX2_AM_RMA_HANDLER, args, nargs,
				      (void *)buf, len, am_flags, NULL, NULL);

		addr += len;
		len_sent += len;
	}

	return 0;
}

static ssize_t psmx2_write(struct fid_ep *ep, const void *buf, size_t len,
			   void *desc, fi_addr_t dest_addr, uint64_t addr,
			   uint64_t key, void *context)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	return psmx2_write_generic(ep, buf, len, desc, dest_addr, addr,
				   key, context, ep_priv->tx_flags, 0);
}

static ssize_t psmx2_writemsg(struct fid_ep *ep,
			      const struct fi_msg_rma *msg,
			      uint64_t flags)
{
	if (!msg || !msg->msg_iov || !msg->iov_count ||
	    !msg->rma_iov || msg->rma_iov_count != 1)
		return -FI_EINVAL;

	if (msg->iov_count > 1)
		return psmx2_writev_generic(ep, msg->msg_iov, msg->desc,
					    msg->iov_count, msg->addr,
					    msg->rma_iov[0].addr,
					    msg->rma_iov[0].key,
					    msg->context, flags, msg->data);

	return psmx2_write_generic(ep, msg->msg_iov[0].iov_base,
				   msg->msg_iov[0].iov_len,
				   msg->desc ? msg->desc[0] : NULL, msg->addr,
				   msg->rma_iov[0].addr, msg->rma_iov[0].key,
				   msg->context, flags, msg->data);
}

static ssize_t psmx2_writev(struct fid_ep *ep, const struct iovec *iov,
		            void **desc, size_t count, fi_addr_t dest_addr,
		            uint64_t addr, uint64_t key, void *context)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	if (!iov || !count)
		return -FI_EINVAL;

	if (count > 1)
		return psmx2_writev_generic(ep, iov, desc, count, dest_addr,
					    addr, key, context, ep_priv->tx_flags, 0);

	return psmx2_write_generic(ep, iov->iov_base, iov->iov_len,
				   desc ? desc[0] : NULL, dest_addr, addr, key,
				   context, ep_priv->tx_flags, 0);
}

static ssize_t psmx2_inject(struct fid_ep *ep, const void *buf, size_t len,
			    fi_addr_t dest_addr, uint64_t addr, uint64_t key)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	return psmx2_write_generic(ep, buf, len, NULL, dest_addr, addr, key, NULL,
				   ep_priv->tx_flags | FI_INJECT | PSMX2_NO_COMPLETION,
				   0);
}

static ssize_t psmx2_writedata(struct fid_ep *ep, const void *buf, size_t len,
			       void *desc, uint64_t data, fi_addr_t dest_addr,
			       uint64_t addr, uint64_t key, void *context)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	return psmx2_write_generic(ep, buf, len, desc, dest_addr, addr, key,
				   context, ep_priv->tx_flags | FI_REMOTE_CQ_DATA,
				   data);
}

static ssize_t psmx2_injectdata(struct fid_ep *ep, const void *buf, size_t len,
			        uint64_t data, fi_addr_t dest_addr, uint64_t addr,
			        uint64_t key)
{
	struct psmx2_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct psmx2_fid_ep, ep);

	return psmx2_write_generic(ep, buf, len, NULL, dest_addr, addr, key, NULL,
				   ep_priv->tx_flags | FI_INJECT | PSMX2_NO_COMPLETION,
				   data);
}

struct fi_ops_rma psmx2_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = psmx2_read,
	.readv = psmx2_readv,
	.readmsg = psmx2_readmsg,
	.write = psmx2_write,
	.writev = psmx2_writev,
	.writemsg = psmx2_writemsg,
	.inject = psmx2_inject,
	.writedata = psmx2_writedata,
	.injectdata = psmx2_injectdata,
};

