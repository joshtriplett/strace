/*
 * Copyright (c) 2020 The strace developers.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "defs.h"
#include "tee.h"
#include "print_fields.h"

#define TEE_FETCH_BUF_DATA(buf_, arg_, params_) \
	tee_fetch_buf_data(tcp, arg, &buf_, sizeof(arg_), \
			   &arg_, &arg_.num_params, \
			   params_)

/* session id is printed as 0x%x in libteec */
#define PRINT_FIELD_SESSION(prefix_, where_, field_) \
	PRINT_FIELD_X(prefix_, where_, field_)

static void
tee_print_buf(struct tee_ioctl_buf_data *buf)
{
	PRINT_FIELD_U("{", *buf, buf_len);
	PRINT_FIELD_ADDR64(", ", *buf, buf_ptr);
	tprints("}");
}

static int
tee_fetch_buf_data(struct tcb *const tcp,
		   const kernel_ulong_t arg,
		   struct tee_ioctl_buf_data *buf,
		   size_t arg_size,
		   void *arg_struct,
		   unsigned *num_params,
		   uint64_t *params)
{
	if (umove_or_printaddr(tcp, arg, buf))
		return RVAL_IOCTL_DECODED;
	if (buf->buf_len > TEE_MAX_ARG_SIZE || buf->buf_len < arg_size) {
		tee_print_buf(buf);
		return RVAL_IOCTL_DECODED;
	}
	if (umoven(tcp, buf->buf_ptr, arg_size, arg_struct)) {
		tee_print_buf(buf);
		return RVAL_IOCTL_DECODED;
	}
	if (entering(tcp) &&
	    (arg_size + TEE_IOCTL_PARAM_SIZE(*num_params) != buf->buf_len)) {
		/*
		 * We could print whatever number of params
		 * is in buf_data, but the kernel would ignore
		 * them anyway (and return -EINVAL) if
		 * the above condition is not satisfied.
		 *
		 * Except for on exiting. The kernel has the right
		 * to update num_params but not buf_len
		 * (see tee_ioctl_supp_recv)
		 */
		tee_print_buf(buf);
		return RVAL_IOCTL_DECODED;
	}
	if (*num_params) {
		*params = buf->buf_ptr + arg_size;
	} else {
		*params = 0;
	}

	return 0;
}

static bool
tee_print_param_fn(struct tcb *tcp, void *elem_buf, size_t elem_size, void *data)
{
	struct tee_ioctl_param *param = (struct tee_ioctl_param *) elem_buf;

	tprintf("{attr=");
	printxval(tee_ioctl_param_attr_types,
		    param->attr & ~TEE_IOCTL_PARAM_ATTR_META,
		    "TEE_IOCTL_PARAM_ATTR_");
	if (param->attr & TEE_IOCTL_PARAM_ATTR_META)
		tprintf("|TEE_IOCTL_PARAM_ATTR_META");

	switch (param->attr) {
	case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
		break;

	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		tprintf(", shm_offs=%#llx", (unsigned long long) param->a);
		tprintf(", size=%#llx", (unsigned long long) param->b);
		tprintf(", shm_id=%llu", (unsigned long long) param->c);
		break;

	case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
	default:
		PRINT_FIELD_X(", ", *param, a);
		PRINT_FIELD_X(", ", *param, b);
		PRINT_FIELD_X(", ", *param, c);
		break;
	}
	tprints("}");
	return true;
}

static void
tee_print_params(struct tcb *const tcp, uint64_t params_start, unsigned num_params)
{
	struct tee_ioctl_param param_buffer;

	tprints(", params=");
	print_array(tcp, params_start, num_params, &param_buffer, sizeof(param_buffer),
		    tfetch_mem, tee_print_param_fn, NULL);
}

static int
tee_version(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_version_data version;

	if (entering(tcp)) {
		tprints(", ");
		return 0;
	}

	if (umove_or_printaddr(tcp, arg, &version))
		return RVAL_IOCTL_DECODED;

	PRINT_FIELD_XVAL("{", version, impl_id,
			 tee_ioctl_impl_ids, "TEE_IMPL_ID_???");
	PRINT_FIELD_FLAGS(", ", version, gen_caps,
			  tee_ioctl_gen_caps, "TEE_GEN_CAP_???");
	if (version.impl_id == TEE_IMPL_ID_OPTEE) {
		PRINT_FIELD_FLAGS(", ", version, impl_caps,
				  tee_ioctl_optee_caps, "TEE_OPTEE_CAP_???");
	} else {
		PRINT_FIELD_X(", ", version, impl_caps);
	}

	tprints("}");
	return RVAL_IOCTL_DECODED;
}

static int
tee_open_session(struct tcb *const tcp, const kernel_ulong_t arg)
{
	int rval;
	struct tee_ioctl_buf_data buf_data;
	struct tee_ioctl_open_session_arg open_session;
	uint64_t params;
	gid_t gid;

	if (entering(tcp)) {
		tprints(", ");

		if ((rval = TEE_FETCH_BUF_DATA(buf_data, open_session, &params)))
			return rval;

		PRINT_FIELD_U("{", buf_data, buf_len);
		PRINT_FIELD_UUID(", buf_ptr={", open_session, uuid);
		PRINT_FIELD_XVAL(", ", open_session, clnt_login,
				 tee_ioctl_login_types, "TEE_IOCTL_LOGIN_???");
		/*
		 * tee_ioctl_open_session_arg.clnt_uuid is used to pass
		 * connectionData, which is currently only used to indicate
		 * which group the client application wishes to authenticate as
		 * (when TEE_IOCTL_LOGIN_GROUP or TEE_IOCTL_LOGIN_GROUP_APPLICATION
		 * are used).
		 *
		 * It is not an UUID; actual client UUID is computed in the kernel.
		 */
		switch (open_session.clnt_login) {
		case TEE_IOCTL_LOGIN_PUBLIC:
		case TEE_IOCTL_LOGIN_USER:
		case TEE_IOCTL_LOGIN_APPLICATION:
		case TEE_IOCTL_LOGIN_USER_APPLICATION:
			break;
		case TEE_IOCTL_LOGIN_GROUP:
		case TEE_IOCTL_LOGIN_GROUP_APPLICATION:
			memcpy(&gid, open_session.clnt_uuid, sizeof(gid));
			printuid(", clnt_uuid=", gid);
			break;
		default:
			PRINT_FIELD_X_ARRAY(", ", open_session, clnt_uuid);
		}
		PRINT_FIELD_U(", ", open_session, cancel_id);
		PRINT_FIELD_U(", ", open_session, num_params);
		tee_print_params(tcp, params, open_session.num_params);

		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		tprints("}");
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		/*
		 * Yes, params are [in/out] for TEE_IOC_OPEN_SESSION.
		 * As for all other operations they are used in.
		 */
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, open_session, &params)))
			return rval;

		PRINT_FIELD_SESSION("{", open_session, session);
		PRINT_FIELD_U(", ", open_session, ret);
		PRINT_FIELD_XVAL(", ", open_session, ret_origin, tee_ioctl_origins,
				 "TEEC_ORIGIN_???");
		tee_print_params(tcp, params, open_session.num_params);

		tprints("}}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_invoke(struct tcb *const tcp, const kernel_ulong_t arg)
{
	int rval;
	struct tee_ioctl_buf_data buf_data;
	struct tee_ioctl_invoke_arg invoke;
	uint64_t params;

	if (entering(tcp)) {
		tprints(", ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, invoke, &params)))
			return rval;

		PRINT_FIELD_U("{", buf_data, buf_len);
		PRINT_FIELD_U(", buf_ptr={", invoke, func);
		PRINT_FIELD_SESSION(", ", invoke, session);
		PRINT_FIELD_U(", ", invoke, cancel_id);
		PRINT_FIELD_U(", ", invoke, num_params);
		tee_print_params(tcp, params, invoke.num_params);

		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		tprints("}");
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, invoke, &params)))
			return rval;

		PRINT_FIELD_U("{", invoke, ret);
		PRINT_FIELD_XVAL(", ", invoke, ret_origin, tee_ioctl_origins,
				 "TEEC_ORIGIN_???");
		tee_print_params(tcp, params, invoke.num_params);

		tprints("}}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_cancel(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_cancel_arg cancel;

	tprints(", ");
	if (umove_or_printaddr(tcp, arg, &cancel))
		return RVAL_IOCTL_DECODED;

	PRINT_FIELD_U("{", cancel, cancel_id);
	PRINT_FIELD_SESSION(", ", cancel, session);

	tprints("}");
	return RVAL_IOCTL_DECODED;
}

static int
tee_close_session(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_close_session_arg close_session;

	tprints(", ");
	if (umove_or_printaddr(tcp, arg, &close_session))
		return RVAL_IOCTL_DECODED;

	PRINT_FIELD_SESSION("{", close_session, session);

	tprints("}");
	return RVAL_IOCTL_DECODED;
}

static int
tee_suppl_recv(struct tcb *const tcp, const kernel_ulong_t arg)
{
	int rval;
	struct tee_ioctl_buf_data buf_data;
	struct tee_iocl_supp_recv_arg supp_recv;
	uint64_t params;

	if (entering(tcp)) {
		tprints(", ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, supp_recv, &params)))
			return rval;

		PRINT_FIELD_U("{", buf_data, buf_len);
		PRINT_FIELD_U(", buf_ptr={", supp_recv, func);
		PRINT_FIELD_U(", ", supp_recv, num_params);
		tee_print_params(tcp, params, supp_recv.num_params);

		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		tprints("}");
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, supp_recv, &params)))
			return rval;

		/* num_params is [in/out] for TEE_IOC_SUPPL_RECV only */
		PRINT_FIELD_U("{", supp_recv, num_params);
		tee_print_params(tcp, params, supp_recv.num_params);

		tprints("}}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_suppl_send(struct tcb *const tcp, const kernel_ulong_t arg)
{
	int rval;
	struct tee_ioctl_buf_data buf_data;
	struct tee_iocl_supp_send_arg supp_send;
	uint64_t params;

	if (entering(tcp)) {
		tprints(", ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, supp_send, &params)))
			return rval;

		PRINT_FIELD_U("{", buf_data, buf_len);
		PRINT_FIELD_U(", buf_ptr={", supp_send, num_params);
		tee_print_params(tcp, params, supp_send.num_params);

		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		tprints("}");
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if ((rval = TEE_FETCH_BUF_DATA(buf_data, supp_send, &params)))
			return rval;

		PRINT_FIELD_U("{", supp_send, ret);
		tee_print_params(tcp, params, supp_send.num_params);

		tprints("}}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_shm_alloc(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_shm_alloc_data shm_alloc;

	if (entering(tcp)) {
		tprints(", ");
		if (umove_or_printaddr(tcp, arg, &shm_alloc))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_X("{", shm_alloc, size);
		PRINT_FIELD_FLAGS(", ", shm_alloc, flags,
				  tee_ioctl_shm_flags, "TEE_IOCTL_SHM_???");
		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if (umove_or_printaddr(tcp, arg, &shm_alloc))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_X("{", shm_alloc, size);
		PRINT_FIELD_FLAGS(", ", shm_alloc, flags,
				  tee_ioctl_shm_flags, "TEE_IOCTL_SHM_???");
		PRINT_FIELD_D(", ", shm_alloc, id);

		tprints("}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_shm_register_fd(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_shm_register_fd_data shm_register_fd;

	if (entering(tcp)) {
		tprints(", ");
		if (umove_or_printaddr(tcp, arg, &shm_register_fd))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_FD("{", shm_register_fd, fd, tcp);
		PRINT_FIELD_FLAGS(", ", shm_register_fd, flags,
				  tee_ioctl_shm_flags, "TEE_IOCTL_SHM_???");
		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if (umove_or_printaddr(tcp, arg, &shm_register_fd))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_X("{", shm_register_fd, size);
		PRINT_FIELD_D(", ", shm_register_fd, id);

		tprints("}");
		return RVAL_IOCTL_DECODED;
	}
}

static int
tee_shm_register(struct tcb *const tcp, const kernel_ulong_t arg)
{
	struct tee_ioctl_shm_register_data shm_register;

	if (entering(tcp)) {
		tprints(", ");
		if (umove_or_printaddr(tcp, arg, &shm_register))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_ADDR64("{", shm_register, addr);
		PRINT_FIELD_X(", ", shm_register, length);
		PRINT_FIELD_FLAGS(", ", shm_register, flags,
				  tee_ioctl_shm_flags, "TEE_IOCTL_SHM_???");
		tprints("}");
		return 0;

	} else if (syserror(tcp)) {
		return RVAL_IOCTL_DECODED;

	} else {
		tprints(" => ");
		if (umove_or_printaddr(tcp, arg, &shm_register))
			return RVAL_IOCTL_DECODED;

		PRINT_FIELD_X("{", shm_register, length);
		PRINT_FIELD_FLAGS(", ", shm_register, flags,
				  tee_ioctl_shm_flags, "TEE_IOCTL_SHM_???");
		PRINT_FIELD_D(", ", shm_register, id);

		tprints("}");
		return RVAL_IOCTL_DECODED;
	}
}

int
tee_ioctl(struct tcb *const tcp, const unsigned int code,
	  const kernel_ulong_t arg)
{
	switch (code) {
	case TEE_IOC_VERSION:
		return tee_version(tcp, arg);

	case TEE_IOC_OPEN_SESSION:
		return tee_open_session(tcp, arg);

	case TEE_IOC_INVOKE:
		return tee_invoke(tcp, arg);

	case TEE_IOC_CANCEL:
		return tee_cancel(tcp, arg);

	case TEE_IOC_CLOSE_SESSION:
		return tee_close_session(tcp, arg);

	case TEE_IOC_SUPPL_RECV:
		return tee_suppl_recv(tcp, arg);

	case TEE_IOC_SUPPL_SEND:
		return tee_suppl_send(tcp, arg);

	case TEE_IOC_SHM_ALLOC:
		return tee_shm_alloc(tcp, arg);

	case TEE_IOC_SHM_REGISTER_FD:
		/* This one isn't upstream */
		return tee_shm_register_fd(tcp, arg);

	case TEE_IOC_SHM_REGISTER:
		return tee_shm_register(tcp, arg);

	default:
		return RVAL_DECODED;
	}
}