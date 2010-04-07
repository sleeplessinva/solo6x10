/*
 * Copyright (C) 2010 Ben Collins <bcollins@bluecherry.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-common.h>
#include <media/videobuf-dma-contig.h>

#include "solo6010.h"

#define SOLO_HW_BPL		2048
#define SOLO_PAGE_SIZE		4
#define SOLO_DISP_PIX_FORMAT	V4L2_PIX_FMT_UYVY
#define SOLO_DISP_PIX_FIELD	V4L2_FIELD_INTERLACED
#define SOLO_DEFAULT_CHAN	0
#define SOLO_DISP_BUF_SIZE	(64 * 1024) // 64k

/* Image size is two fields, SOLO_HW_BPL is one horizontal line */
#define solo_vlines(__solo)	(__solo->video_vsize * 2)
#define solo_image_size(__solo) (solo_bytesperline(__solo) * \
				 solo_vlines(__solo))
#define solo_bytesperline(__solo) (__solo->video_hsize * 2)

#define MIN_VID_BUFFERS		4

/* Simple file handle */
struct solo_filehandle {
	struct solo6010_dev	*solo_dev;
	struct videobuf_queue	vidq;
	struct task_struct      *kthread;
	spinlock_t		slock;
	int			old_write;
	struct list_head	vidq_active;
	wait_queue_head_t	thread_wait;
};

unsigned video_nr = -1;
module_param(video_nr, uint, 0644);
MODULE_PARM_DESC(video_nr, "videoX start number, -1 is autodetect (default)");

static void erase_on(struct solo6010_dev *solo_dev)
{
	solo_reg_write(solo_dev, SOLO_VO_DISP_ERASE, SOLO_VO_DISP_ERASE_ON);
	solo_dev->erasing = 1;
	solo_dev->frame_blank = 0;
}

static int erase_off(struct solo6010_dev *solo_dev)
{
	if (!solo_dev->erasing)
		return 0;

	/* First time around, assert erase off */
	if (!solo_dev->frame_blank)
		solo_reg_write(solo_dev, SOLO_VO_DISP_ERASE, 0);
	/* Keep the erasing flag on for 8 frames minimum */
	if (solo_dev->frame_blank++ >= 8)
		solo_dev->erasing = 0;

	return 1;
}

static int solo_v4l2_ch(struct solo6010_dev *solo_dev, u8 ch, int on)
{
	if (ch >= solo_dev->nr_chans)
		return -EINVAL;

	/* Here, we just keep window/channel the same */
	solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL0(ch),
		       SOLO_VI_WIN_CHANNEL(ch) |
		       SOLO_VI_WIN_SX(on ? 0 : solo_dev->video_hsize) |
		       SOLO_VI_WIN_EX(solo_dev->video_hsize) |
		       SOLO_VI_WIN_SCALE(on ? 1 : 0));

	solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL1(ch),
		       SOLO_VI_WIN_SY(on ? 0 : solo_vlines(solo_dev)) |
		       SOLO_VI_WIN_EY(solo_vlines(solo_dev)));

	solo_reg_write(solo_dev, SOLO_VI_WIN_ON(ch), on ? 1 : 0);

	return 0;
}

static int solo_v4l2_set_ch(struct solo6010_dev *solo_dev, unsigned int ch)
{
	if (ch >= solo_dev->nr_chans)
		return -EINVAL;

	erase_on(solo_dev);

	solo_v4l2_ch(solo_dev, solo_dev->cur_disp_ch, 0);
	solo_v4l2_ch(solo_dev, ch, 1);

	solo_dev->cur_disp_ch = ch;

	return 0;
}

static void solo_fillbuf(struct solo_filehandle *fh,
			 struct videobuf_buffer *vb)
{
	struct solo6010_dev *solo_dev = fh->solo_dev;
	dma_addr_t vbuf;
	unsigned int fdma_addr;
	int cur_write;
	int frame_size;
	int error = 1;
	int i;

	if (!(vbuf = videobuf_to_dma_contig(vb)))
		goto finish_buf;

	/* XXX: Is this really a good idea? */
	do {
		unsigned int status = solo_reg_read(solo_dev, SOLO_VI_STATUS0);
		cur_write = SOLO_VI_STATUS0_PAGE(status);
		if (cur_write != fh->old_write)
			break;
		msleep_interruptible(1);
	} while(1);

	fh->old_write = cur_write;

	if (erase_off(solo_dev)) {
		void *p = videobuf_queue_to_vmalloc(&fh->vidq, vb);
		int image_size = solo_image_size(solo_dev);
		for (i = 0; i < image_size; i += 2) {
			((u8 *)p)[i] = 0x80;
			((u8 *)p)[i + 1] = 0x00;
		}
		error = 0;
		goto finish_buf;
	}

	frame_size = SOLO_HW_BPL * solo_vlines(solo_dev);
	fdma_addr = SOLO_DISP_EXT_ADDR(solo_dev) + (cur_write * frame_size);

	for (i = 0; i < frame_size / SOLO_DISP_BUF_SIZE; i++) {
		int j;
		for (j = 0; j < (SOLO_DISP_BUF_SIZE / SOLO_HW_BPL); j++) {
			if (solo_p2m_dma_t(solo_dev, SOLO_P2M_DMA_ID_DISP, 0,
					   vbuf, fdma_addr + (j * SOLO_HW_BPL),
					   solo_bytesperline(solo_dev)))
				goto finish_buf;
			vbuf += solo_bytesperline(solo_dev);
		}
		fdma_addr += SOLO_DISP_BUF_SIZE;
	}
	error = 0;

finish_buf:
	if (error) {
		vb->state = VIDEOBUF_ERROR;
	} else {
		vb->state = VIDEOBUF_DONE;
		vb->field_count++;
		do_gettimeofday(&vb->ts);
	}

	wake_up(&vb->done);

	return;
}

static void solo_thread_try(struct solo_filehandle *fh)
{
	struct videobuf_buffer *vb;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&fh->slock, flags);

		if (list_empty(&fh->vidq_active))
			break;

		vb = list_first_entry(&fh->vidq_active, struct videobuf_buffer,
				      queue);

		if (!waitqueue_active(&vb->done))
			break;

		list_del(&vb->queue);
		spin_unlock_irqrestore(&fh->slock, flags);

		solo_fillbuf(fh, vb);
	}

	spin_unlock_irqrestore(&fh->slock, flags);
}

static int solo_thread(void *data)
{
	struct solo_filehandle *fh = data;
	DECLARE_WAITQUEUE(wait, current);
	long timeout;

	set_freezable();
	add_wait_queue(&fh->thread_wait, &wait);

	for (;;) {
		timeout = HZ;
		if (list_empty(&fh->vidq_active))
			timeout = schedule_timeout_interruptible(timeout);
		if (timeout == -ERESTARTSYS || kthread_should_stop())
			break;
		solo_thread_try(fh);
		try_to_freeze();
	}

	remove_wait_queue(&fh->thread_wait, &wait);

        return 0;
}

static int solo_start_thread(struct solo_filehandle *fh)
{
	fh->kthread = kthread_run(solo_thread, fh, SOLO6010_NAME "_disp");

	if (IS_ERR(fh->kthread))
		return PTR_ERR(fh->kthread);

	return 0;
}

static void solo_stop_thread(struct solo_filehandle *fh)
{
	if (fh->kthread) {
		kthread_stop(fh->kthread);
		fh->kthread = NULL;
	}
}

static int solo_buf_setup(struct videobuf_queue *vq, unsigned int *count,
			  unsigned int *size)
{
	struct solo_filehandle *fh = vq->priv_data;
	struct solo6010_dev *solo_dev  = fh->solo_dev;

        *size = solo_image_size(solo_dev);

        if (*count < MIN_VID_BUFFERS)
		*count = MIN_VID_BUFFERS;

        return 0;
}

static int solo_buf_prepare(struct videobuf_queue *vq,
			    struct videobuf_buffer *vb, enum v4l2_field field)
{
	struct solo_filehandle *fh  = vq->priv_data;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	vb->size = solo_image_size(solo_dev);
	if (vb->baddr != 0 && vb->bsize < vb->size)
		return -EINVAL;

	/* XXX: These properties only change when queue is idle */
	vb->width  = solo_dev->video_hsize;
	vb->height = solo_vlines(solo_dev);
	vb->bytesperline = solo_bytesperline(solo_dev);
	vb->field  = field;

	if (vb->state == VIDEOBUF_NEEDS_INIT) {
		int rc = videobuf_iolock(vq, vb, NULL);
		if (rc < 0) {
			videobuf_dma_contig_free(vq, vb);
			vb->state = VIDEOBUF_NEEDS_INIT;
			return rc;
		}
	}
	vb->state = VIDEOBUF_PREPARED;

	return 0;
}

static void solo_buf_queue(struct videobuf_queue *vq,
			   struct videobuf_buffer *vb)
{
	struct solo_filehandle *fh = vq->priv_data;

	vb->state = VIDEOBUF_QUEUED;
	list_add_tail(&vb->queue, &fh->vidq_active);
	wake_up_interruptible(&fh->thread_wait);
}

static void solo_buf_release(struct videobuf_queue *vq,
			     struct videobuf_buffer *vb)
{
	videobuf_dma_contig_free(vq, vb);
	vb->state = VIDEOBUF_NEEDS_INIT;
}

static struct videobuf_queue_ops solo_video_qops = {
	.buf_setup	= solo_buf_setup,
	.buf_prepare	= solo_buf_prepare,
	.buf_queue	= solo_buf_queue,
	.buf_release	= solo_buf_release,
};

static unsigned int solo_v4l2_poll(struct file *file,
				   struct poll_table_struct *wait)
{
	struct solo_filehandle *fh = file->private_data;

        return videobuf_poll_stream(file, &fh->vidq, wait);
}

static int solo_v4l2_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct solo_filehandle *fh = file->private_data;

	return videobuf_mmap_mapper(&fh->vidq, vma);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static int solo_v4l2_open(struct file *file)
#else
static int solo_v4l2_open(struct inode *ino, struct file *file)
#endif
{
	struct solo6010_dev *solo_dev = video_drvdata(file);
	struct solo_filehandle *fh;
	int ret;

	if ((fh = kzalloc(sizeof(*fh), GFP_KERNEL)) == NULL)
		return -ENOMEM;

	spin_lock_init(&fh->slock);
	INIT_LIST_HEAD(&fh->vidq_active);
	init_waitqueue_head(&fh->thread_wait);
	fh->solo_dev = solo_dev;
	file->private_data = fh;

	if ((ret = solo_start_thread(fh))) {
		kfree(fh);
		return ret;
	}

	videobuf_queue_dma_contig_init(&fh->vidq, &solo_video_qops,
				    &solo_dev->pdev->dev, &fh->slock,
				    V4L2_BUF_TYPE_VIDEO_CAPTURE,
				    SOLO_DISP_PIX_FIELD,
				    sizeof(struct videobuf_buffer), fh);

	return 0;
}

static ssize_t solo_v4l2_read(struct file *file, char __user *data,
			      size_t count, loff_t *ppos)
{
	struct solo_filehandle *fh = file->private_data;

	return videobuf_read_stream(&fh->vidq, data, count, ppos, 0,
				    file->f_flags & O_NONBLOCK);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static int solo_v4l2_release(struct file *file)
#else
static int solo_v4l2_release(struct inode *ino, struct file *file)
#endif
{
	struct solo_filehandle *fh = file->private_data;

	videobuf_stop(&fh->vidq);
	videobuf_mmap_free(&fh->vidq);
	solo_stop_thread(fh);
	kfree(fh);

	return 0;
}

static int solo_querycap(struct file *file, void  *priv,
			 struct v4l2_capability *cap)
{
	struct solo_filehandle  *fh  = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	strcpy(cap->driver, SOLO6010_NAME);
	strcpy(cap->card, "Softlogic 6010");
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI %s",
		 pci_name(solo_dev->pdev));
	cap->version = SOLO6010_VER_NUM;
	cap->capabilities =     V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_READWRITE |
				V4L2_CAP_STREAMING;
	return 0;
}

static int solo_enum_input(struct file *file, void *priv,
			   struct v4l2_input *input)
{
	struct solo_filehandle *fh  = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	if (input->index >= solo_dev->nr_chans)
		return -EINVAL;

	snprintf(input->name, sizeof(input->name), "Camera %d",
		 input->index + 1);
	input->type = V4L2_INPUT_TYPE_CAMERA;

	if (solo_dev->video_type == SOLO_VO_FMT_TYPE_NTSC)
		input->std = V4L2_STD_NTSC_M;
	else
		input->std = V4L2_STD_PAL_M;

	/* XXX Should check for signal status on this camera */
	input->status = 0;

	return 0;
}

static int solo_set_input(struct file *file, void *priv, unsigned int index)
{
	struct solo_filehandle *fh = priv;

	return solo_v4l2_set_ch(fh->solo_dev, index);
}

static int solo_get_input(struct file *file, void *priv, unsigned int *index)
{
	struct solo_filehandle *fh = priv;

	*index = fh->solo_dev->cur_disp_ch;

	return 0;
}

static int solo_enum_fmt_cap(struct file *file, void *priv,
			     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = SOLO_DISP_PIX_FORMAT;
	snprintf(f->description, sizeof(f->description),
		 "%s", "UYUV 4:2:2 Packed");

	return 0;
}

static int solo_try_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	int image_size = solo_image_size(solo_dev);

	/* Check supported sizes */
	if (pix->width != solo_dev->video_hsize)
		pix->width = solo_dev->video_hsize;
	if (pix->height != solo_vlines(solo_dev))
		pix->height = solo_vlines(solo_dev);
	if (pix->sizeimage != image_size)
		pix->sizeimage = image_size;

	/* Check formats */
	if (pix->field == V4L2_FIELD_ANY)
		pix->field = SOLO_DISP_PIX_FIELD;

	if (pix->pixelformat != SOLO_DISP_PIX_FORMAT ||
	    pix->field       != SOLO_DISP_PIX_FIELD ||
	    pix->colorspace  != V4L2_COLORSPACE_SMPTE170M)
		return -EINVAL;

	return 0;
}

static int solo_set_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;

	if (videobuf_queue_is_busy(&fh->vidq))
		return -EBUSY;

	/* For right now, if it doesn't match our running config,
	 * then fail */
	return solo_try_fmt_cap(file, priv, f);
}

static int solo_get_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->width = solo_dev->video_hsize;
	pix->height = solo_vlines(solo_dev);
	pix->pixelformat = SOLO_DISP_PIX_FORMAT;
	pix->field = SOLO_DISP_PIX_FIELD;
	pix->sizeimage = solo_image_size(solo_dev);
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
	pix->bytesperline = solo_bytesperline(solo_dev);

	return 0;
}

static int solo_reqbufs(struct file *file, void *priv, 
			struct v4l2_requestbuffers *req)
{
	struct solo_filehandle *fh = priv;

	return videobuf_reqbufs(&fh->vidq, req);
}

static int solo_querybuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct solo_filehandle *fh = priv;

	return videobuf_querybuf(&fh->vidq, buf);
}

static int solo_qbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct solo_filehandle *fh = priv;

	return videobuf_qbuf(&fh->vidq, buf);
}

static int solo_dqbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct solo_filehandle *fh = priv;

	return videobuf_dqbuf(&fh->vidq, buf, file->f_flags & O_NONBLOCK);
}

static int solo_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct solo_filehandle *fh = priv;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return videobuf_streamon(&fh->vidq);
}

static int solo_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct solo_filehandle *fh = priv;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return videobuf_streamoff(&fh->vidq);
}

static int solo_s_std(struct file *file, void *priv, v4l2_std_id *i)
{
	return 0;
}

static const u32 solo_motion_ctrls[] = {
	V4L2_CID_MOTION_TRACE,
	0
};

static const u32 *solo_ctrl_classes[] = {
	solo_motion_ctrls,
	NULL
};

static int solo_disp_queryctrl(struct file *file, void *priv,
			       struct v4l2_queryctrl *qc)
{
	qc->id = v4l2_ctrl_next(solo_ctrl_classes, qc->id);
	if (!qc->id)
		return -EINVAL;

	switch (qc->id) {
#ifdef PRIVATE_CIDS
	case V4L2_CID_MOTION_TRACE:
		qc->type = V4L2_CTRL_TYPE_BOOLEAN;
		qc->minimum = 0;
		qc->maximum = qc->step = 1;
		qc->default_value = 0;
		strlcpy(qc->name, "Motion Detection Trace", sizeof(qc->name));
		return 0;
#else
	case V4L2_CID_MOTION_TRACE:
		return v4l2_ctrl_query_fill(qc, 0, 1, 1, 0);
#endif
	}
	return -EINVAL;
}

static int solo_disp_g_ctrl(struct file *file, void *priv,
			    struct v4l2_control *ctrl)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	switch (ctrl->id) {
	case V4L2_CID_MOTION_TRACE:
		ctrl->value = solo_reg_read(solo_dev, SOLO_VI_MOTION_BAR)
			? 1 : 0;
		return 0;
	}
	return -EINVAL;
}

static int solo_disp_s_ctrl(struct file *file, void *priv,
			    struct v4l2_control *ctrl)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	switch (ctrl->id) {
	case V4L2_CID_MOTION_TRACE:
		if (ctrl->value) {
			solo_reg_write(solo_dev, SOLO_VI_MOTION_BORDER,
					SOLO_VI_MOTION_Y_ADD |
					SOLO_VI_MOTION_Y_VALUE(0x20) |
					SOLO_VI_MOTION_CB_VALUE(0x10) |
					SOLO_VI_MOTION_CR_VALUE(0x10));
			solo_reg_write(solo_dev, SOLO_VI_MOTION_BAR,
					SOLO_VI_MOTION_CR_ADD |
					SOLO_VI_MOTION_Y_VALUE(0x10) |
					SOLO_VI_MOTION_CB_VALUE(0x80) |
					SOLO_VI_MOTION_CR_VALUE(0x10));
		} else {
			solo_reg_write(solo_dev, SOLO_VI_MOTION_BORDER, 0);
			solo_reg_write(solo_dev, SOLO_VI_MOTION_BAR, 0);
		}
		return 0;
	}
	return -EINVAL;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static const struct v4l2_file_operations solo_v4l2_fops = {
#else
static const struct file_operations solo_v4l2_fops = {
#endif
	.owner			= THIS_MODULE,
	.open			= solo_v4l2_open,
	.release		= solo_v4l2_release,
	.read			= solo_v4l2_read,
	.poll			= solo_v4l2_poll,
	.mmap			= solo_v4l2_mmap,
	.ioctl			= video_ioctl2,
};

static const struct v4l2_ioctl_ops solo_v4l2_ioctl_ops = {
	.vidioc_querycap		= solo_querycap,
	.vidioc_s_std			= solo_s_std,
	/* Input callbacks */
	.vidioc_enum_input		= solo_enum_input,
	.vidioc_s_input			= solo_set_input,
	.vidioc_g_input			= solo_get_input,
	/* Video capture format callbacks */
	.vidioc_enum_fmt_vid_cap	= solo_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap		= solo_try_fmt_cap,
	.vidioc_s_fmt_vid_cap		= solo_set_fmt_cap,
	.vidioc_g_fmt_vid_cap		= solo_get_fmt_cap,
	/* Streaming I/O */
	.vidioc_reqbufs			= solo_reqbufs,
	.vidioc_querybuf		= solo_querybuf,
	.vidioc_qbuf			= solo_qbuf,
	.vidioc_dqbuf			= solo_dqbuf,
	.vidioc_streamon		= solo_streamon,
        .vidioc_streamoff		= solo_streamoff,
	/* Controls */
	.vidioc_queryctrl		= solo_disp_queryctrl,
        .vidioc_g_ctrl			= solo_disp_g_ctrl,
        .vidioc_s_ctrl			= solo_disp_s_ctrl,
};

static struct video_device solo_v4l2_template = {
	.name			= SOLO6010_NAME,
	.fops			= &solo_v4l2_fops,
	.ioctl_ops		= &solo_v4l2_ioctl_ops,
	.minor			= -1,
	.release		= video_device_release,

	.tvnorms		= V4L2_STD_NTSC_M | V4L2_STD_PAL_M,
	.current_norm		= V4L2_STD_NTSC_M,
};

int solo_v4l2_init(struct solo6010_dev *solo_dev)
{
	int ret;
	int i;

	solo_dev->vfd = video_device_alloc();
	if (!solo_dev->vfd)
		return -ENOMEM;

	*solo_dev->vfd = solo_v4l2_template;
	solo_dev->vfd->parent = &solo_dev->pdev->dev;

	ret = video_register_device(solo_dev->vfd, VFL_TYPE_GRABBER, video_nr);
	if (ret < 0) {
		video_device_release(solo_dev->vfd);
		solo_dev->vfd = NULL;
		return ret;
	}

	video_set_drvdata(solo_dev->vfd, solo_dev);

	snprintf(solo_dev->vfd->name, sizeof(solo_dev->vfd->name), "%s (%i)",
		 SOLO6010_NAME, solo_dev->vfd->num);

	if (video_nr >= 0)
		video_nr++;

	dev_info(&solo_dev->pdev->dev, "Display as /dev/video%d with "
		 "%d inputs\n", solo_dev->vfd->num, solo_dev->nr_chans);

	/* Set the default display channel */
	for (i = 0; i < solo_dev->nr_chans; i++) {
		solo_v4l2_set_ch(solo_dev, i);
		while (erase_off(solo_dev))
			;// Do nothing
	}
	solo_v4l2_set_ch(solo_dev, SOLO_DEFAULT_CHAN);
	while(erase_off(solo_dev))
		;// Do nothing

	return 0;
}

void solo_v4l2_exit(struct solo6010_dev *solo_dev)
{
	video_unregister_device(solo_dev->vfd);
	solo_dev->vfd = NULL;
}
