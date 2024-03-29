#ifndef __LINUX__AIO_H
#define __LINUX__AIO_H

#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/aio_abi.h>
#include <linux/uio.h>
#include <linux/rcupdate.h>
#include <linux/atomic.h>
#include <linux/batch_complete.h>

struct kioctx;
struct kiocb;
struct batch_complete;

#define KIOCB_KEY		0

/*
 * We use ki_cancel == KIOCB_CANCELLED to indicate that a kiocb has been either
 * cancelled or completed (this makes a certain amount of sense because
 * successful cancellation - io_cancel() - does deliver the completion to
 * userspace).
 *
 * And since most things don't implement kiocb cancellation and we'd really like
 * kiocb completion to be lockless when possible, we use ki_cancel to
 * synchronize cancellation and completion - we only set it to KIOCB_CANCELLED
 * with xchg() or cmpxchg(), see batch_complete_aio() and kiocb_cancel().
 */
#define KIOCB_CANCELLED		((void *) (~0ULL))

typedef int (kiocb_cancel_fn)(struct kiocb *, struct io_event *);

struct kiocb {
	struct rb_node		ki_node;

	atomic_t		ki_users;

	struct file		*ki_filp;
	struct kioctx		*ki_ctx;	/* NULL for sync ops */
	kiocb_cancel_fn		*ki_cancel;
	void			(*ki_dtor)(struct kiocb *);

	union {
		void __user		*user;
		struct task_struct	*tsk;
	} ki_obj;

	__u64			ki_user_data;	/* user's data for completion */
	long			ki_res;
	long			ki_res2;

	loff_t			ki_pos;

	void			*private;
	/* State that we remember to be able to restart/retry  */
	unsigned short		ki_opcode;
	size_t			ki_nbytes; 	/* copy of iocb->aio_nbytes */
	char 			__user *ki_buf;	/* remaining iocb->aio_buf */
	size_t			ki_left; 	/* remaining bytes */
	struct iovec		ki_inline_vec;	/* inline vector */
 	struct iovec		*ki_iovec;
 	unsigned long		ki_nr_segs;
 	unsigned long		ki_cur_seg;

	struct list_head	ki_list;	/* the aio core uses this
						 * for cancellation */

	/*
	 * If the aio_resfd field of the userspace iocb is not zero,
	 * this is the underlying eventfd context to deliver events to.
	 */
	struct eventfd_ctx	*ki_eventfd;
};

static inline bool is_sync_kiocb(struct kiocb *kiocb)
{
	return kiocb->ki_ctx == NULL;
}

static inline void init_sync_kiocb(struct kiocb *kiocb, struct file *filp)
{
	*kiocb = (struct kiocb) {
			.ki_users = ATOMIC_INIT(1),
			.ki_ctx = NULL,
			.ki_filp = filp,
			.ki_obj.tsk = current,
		};
}

/* prototypes */
#ifdef CONFIG_AIO
extern ssize_t wait_on_sync_kiocb(struct kiocb *iocb);
extern void aio_put_req(struct kiocb *iocb);
extern void batch_complete_aio(struct batch_complete *batch);
extern void aio_complete_batch(struct kiocb *iocb, long res, long res2,
			       struct batch_complete *batch);
struct mm_struct;
extern void exit_aio(struct mm_struct *mm);
extern long do_io_submit(aio_context_t ctx_id, long nr,
			 struct iocb __user *__user *iocbpp, bool compat);
void kiocb_set_cancel_fn(struct kiocb *req, kiocb_cancel_fn *cancel);
#else
static inline ssize_t wait_on_sync_kiocb(struct kiocb *iocb) { return 0; }
static inline void aio_put_req(struct kiocb *iocb) { }

static inline void batch_complete_aio(struct batch_complete *batch) { }
static inline void aio_complete_batch(struct kiocb *iocb, long res, long res2,
				      struct batch_complete *batch)
{
	return;
}
struct mm_struct;
static inline void exit_aio(struct mm_struct *mm) { }
static inline long do_io_submit(aio_context_t ctx_id, long nr,
				struct iocb __user * __user *iocbpp,
				bool compat) { return 0; }
static inline void kiocb_set_cancel_fn(struct kiocb *req,
				       kiocb_cancel_fn *cancel) { }
#endif /* CONFIG_AIO */

static inline void aio_complete(struct kiocb *iocb, long res, long res2)
{
	aio_complete_batch(iocb, res, res2, NULL);
}

static inline struct kiocb *list_kiocb(struct list_head *h)
{
	return list_entry(h, struct kiocb, ki_list);
}

/* for sysctl: */
extern unsigned long aio_nr;
extern unsigned long aio_max_nr;

#endif /* __LINUX__AIO_H */
