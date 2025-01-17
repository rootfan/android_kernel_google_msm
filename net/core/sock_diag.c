#include <linux/mutex.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <net/netlink.h>
#include <net/net_namespace.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <net/sock.h>

#include <linux/inet_diag.h>
#include <linux/sock_diag.h>

static struct sock_diag_handler *sock_diag_handlers[AF_MAX];
static int (*inet_rcv_compat)(struct sk_buff *skb, struct nlmsghdr *nlh);
static DEFINE_MUTEX(sock_diag_table_mutex);

u64 sock_gen_cookie(struct sock *sk)
{
	while (1) {
		u64 res = atomic64_read(&sk->sk_cookie);

		if (res)
			return res;
		res = atomic64_inc_return(&sock_net(sk)->cookie_gen);
		atomic64_cmpxchg(&sk->sk_cookie, 0, res);
	}
}

int sock_diag_check_cookie(struct sock *sk, const __u32 *cookie)
{
	u64 res;

	if (cookie[0] == INET_DIAG_NOCOOKIE && cookie[1] == INET_DIAG_NOCOOKIE)
		return 0;

	res = sock_gen_cookie(sk);
	if ((u32)res != cookie[0] || (u32)(res >> 32) != cookie[1])
		return -ESTALE;

	return 0;
}
EXPORT_SYMBOL_GPL(sock_diag_check_cookie);

void sock_diag_save_cookie(struct sock *sk, __u32 *cookie)
{
	u64 res = sock_gen_cookie(sk);

	cookie[0] = (u32)res;
	cookie[1] = (u32)(res >> 32);
}
EXPORT_SYMBOL_GPL(sock_diag_save_cookie);

int sock_diag_put_meminfo(struct sock *sk, struct sk_buff *skb, int attrtype)
{
	__u32 *mem;

	mem = RTA_DATA(__RTA_PUT(skb, attrtype, SK_MEMINFO_VARS * sizeof(__u32)));

	mem[SK_MEMINFO_RMEM_ALLOC] = sk_rmem_alloc_get(sk);
	mem[SK_MEMINFO_RCVBUF] = sk->sk_rcvbuf;
	mem[SK_MEMINFO_WMEM_ALLOC] = sk_wmem_alloc_get(sk);
	mem[SK_MEMINFO_SNDBUF] = sk->sk_sndbuf;
	mem[SK_MEMINFO_FWD_ALLOC] = sk->sk_forward_alloc;
	mem[SK_MEMINFO_WMEM_QUEUED] = sk->sk_wmem_queued;
	mem[SK_MEMINFO_OPTMEM] = atomic_read(&sk->sk_omem_alloc);

	return 0;

rtattr_failure:
	return -EMSGSIZE;
}
EXPORT_SYMBOL_GPL(sock_diag_put_meminfo);

void sock_diag_register_inet_compat(int (*fn)(struct sk_buff *skb, struct nlmsghdr *nlh))
{
	mutex_lock(&sock_diag_table_mutex);
	inet_rcv_compat = fn;
	mutex_unlock(&sock_diag_table_mutex);
}
EXPORT_SYMBOL_GPL(sock_diag_register_inet_compat);

void sock_diag_unregister_inet_compat(int (*fn)(struct sk_buff *skb, struct nlmsghdr *nlh))
{
	mutex_lock(&sock_diag_table_mutex);
	inet_rcv_compat = NULL;
	mutex_unlock(&sock_diag_table_mutex);
}
EXPORT_SYMBOL_GPL(sock_diag_unregister_inet_compat);

int sock_diag_register(struct sock_diag_handler *hndl)
{
	int err = 0;

	if (hndl->family >= AF_MAX)
		return -EINVAL;

	mutex_lock(&sock_diag_table_mutex);
	if (sock_diag_handlers[hndl->family])
		err = -EBUSY;
	else
		sock_diag_handlers[hndl->family] = hndl;
	mutex_unlock(&sock_diag_table_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(sock_diag_register);

void sock_diag_unregister(struct sock_diag_handler *hnld)
{
	int family = hnld->family;

	if (family >= AF_MAX)
		return;

	mutex_lock(&sock_diag_table_mutex);
	BUG_ON(sock_diag_handlers[family] != hnld);
	sock_diag_handlers[family] = NULL;
	mutex_unlock(&sock_diag_table_mutex);
}
EXPORT_SYMBOL_GPL(sock_diag_unregister);

static inline struct sock_diag_handler *sock_diag_lock_handler(int family)
{
	if (sock_diag_handlers[family] == NULL)
		request_module("net-pf-%d-proto-%d-type-%d", PF_NETLINK,
				NETLINK_SOCK_DIAG, family);

	mutex_lock(&sock_diag_table_mutex);
	return sock_diag_handlers[family];
}

static inline void sock_diag_unlock_handler(struct sock_diag_handler *h)
{
	mutex_unlock(&sock_diag_table_mutex);
}

static int __sock_diag_cmd(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int err;
	struct sock_diag_req *req = NLMSG_DATA(nlh);
	struct sock_diag_handler *hndl;

	if (nlmsg_len(nlh) < sizeof(*req))
		return -EINVAL;

	if (req->sdiag_family >= AF_MAX)
		return -EINVAL;

	hndl = sock_diag_lock_handler(req->sdiag_family);
	if (hndl == NULL)
		err = -ENOENT;
	else if (nlh->nlmsg_type == SOCK_DIAG_BY_FAMILY)
		err = hndl->dump(skb, nlh);
	else if (nlh->nlmsg_type == SOCK_DESTROY_BACKPORT && hndl->destroy)
		err = hndl->destroy(skb, nlh);
	else
		err = -EOPNOTSUPP;
	sock_diag_unlock_handler(hndl);

	return err;
}

static int sock_diag_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int ret;

	switch (nlh->nlmsg_type) {
	case TCPDIAG_GETSOCK:
	case DCCPDIAG_GETSOCK:
		if (inet_rcv_compat == NULL)
			request_module("net-pf-%d-proto-%d-type-%d", PF_NETLINK,
					NETLINK_SOCK_DIAG, AF_INET);

		mutex_lock(&sock_diag_table_mutex);
		if (inet_rcv_compat != NULL)
			ret = inet_rcv_compat(skb, nlh);
		else
			ret = -EOPNOTSUPP;
		mutex_unlock(&sock_diag_table_mutex);

		return ret;
	case SOCK_DIAG_BY_FAMILY:
	case SOCK_DESTROY_BACKPORT:
		return __sock_diag_cmd(skb, nlh);
	default:
		return -EINVAL;
	}
}

static DEFINE_MUTEX(sock_diag_mutex);

static void sock_diag_rcv(struct sk_buff *skb)
{
	mutex_lock(&sock_diag_mutex);
	netlink_rcv_skb(skb, &sock_diag_rcv_msg);
	mutex_unlock(&sock_diag_mutex);
}

int sock_diag_destroy(struct sock *sk, int err)
{
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (!sk->sk_prot->diag_destroy)
		return -EOPNOTSUPP;

	return sk->sk_prot->diag_destroy(sk, err);
}
EXPORT_SYMBOL_GPL(sock_diag_destroy);

struct sock *sock_diag_nlsk;
EXPORT_SYMBOL_GPL(sock_diag_nlsk);

static int __init sock_diag_init(void)
{
	sock_diag_nlsk = netlink_kernel_create(&init_net, NETLINK_SOCK_DIAG, 0,
					sock_diag_rcv, NULL, THIS_MODULE);
	return sock_diag_nlsk == NULL ? -ENOMEM : 0;
}

static void __exit sock_diag_exit(void)
{
	netlink_kernel_release(sock_diag_nlsk);
}

module_init(sock_diag_init);
module_exit(sock_diag_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS_NET_PF_PROTO(PF_NETLINK, NETLINK_SOCK_DIAG);
