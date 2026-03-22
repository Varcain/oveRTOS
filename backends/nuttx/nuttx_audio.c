/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/audio.h"
#include "ove_backend_common.h"

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <errno.h>
#include <mqueue.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>
#include <nuttx/queue.h>

#define AUDIO_APB_RECORD (1 << 4)
#define AUDIO_APB_PLAY   (1 << 5)

#define AUDIO_PLAY_PATH  "/dev/audio/pcm0"
#define AUDIO_REC_PATH   "/dev/audio/pcm1"
#define DEFAULT_NUM_BUFFERS   3
#define DEFAULT_AUDIO_PRIORITY 200
#define DEFAULT_AUDIO_STACK    4096
#define BYTES_PER_SAMPLE  sizeof(int16_t)

static ove_audio_process_fn g_process_fn;
static void *g_user_data;
static unsigned int g_frames_per_buffer;
static unsigned int g_num_buffers;
static unsigned int g_thread_priority;
static unsigned int g_thread_stack_size;

static int record_fd = -1;
static int play_fd = -1;
static mqd_t audio_mq = (mqd_t)-1;
static sem_t audio_ready_sem;
static pthread_t audio_tid;

/* ========================================================================= */
/* AUDIO THREAD                                                              */
/* ========================================================================= */

static void *audio_thread_fn(void *arg)
{
  struct audio_buf_desc_s buf_desc;
  struct ap_buffer_info_s rec_buf_info;
  struct ap_buffer_info_s play_buf_info;
  struct ap_buffer_s **rec_bufs = NULL;
  struct ap_buffer_s **play_bufs = NULL;
  struct audio_msg_s msg;
  unsigned int prio;
  int rec_nbuffers;
  int play_nbuffers;
  int rec_buf_size;
  int play_buf_size;
  int block_size;
  int ret;
  int i;

  (void)arg;

  block_size = g_frames_per_buffer * BYTES_PER_SAMPLE;

  sem_wait(&audio_ready_sem);
  printf("Audio thread running\n");

  /* Query buffer info */

  ret = ioctl(record_fd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)&rec_buf_info);
  if (ret != OK)
    {
      rec_buf_info.buffer_size = block_size;
      rec_buf_info.nbuffers = g_num_buffers;
    }

  rec_nbuffers = rec_buf_info.nbuffers;
  rec_buf_size = rec_buf_info.buffer_size;
  if (rec_nbuffers > g_num_buffers)
    {
      rec_nbuffers = g_num_buffers;
    }

  ret = ioctl(play_fd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)&play_buf_info);
  if (ret != OK)
    {
      play_buf_info.buffer_size = block_size;
      play_buf_info.nbuffers = g_num_buffers;
    }

  play_nbuffers = play_buf_info.nbuffers;
  play_buf_size = play_buf_info.buffer_size;
  if (play_nbuffers > g_num_buffers)
    {
      play_nbuffers = g_num_buffers;
    }

  /* Allocate buffers */

  rec_bufs = OVE_BACKEND_MALLOC(rec_nbuffers * sizeof(struct ap_buffer_s *));
  play_bufs = OVE_BACKEND_MALLOC(play_nbuffers * sizeof(struct ap_buffer_s *));
  if (rec_bufs != NULL)
    {
      memset(rec_bufs, 0, rec_nbuffers * sizeof(struct ap_buffer_s *));
    }
  if (play_bufs != NULL)
    {
      memset(play_bufs, 0, play_nbuffers * sizeof(struct ap_buffer_s *));
    }
  if (rec_bufs == NULL || play_bufs == NULL)
    {
      goto err_out;
    }

  for (i = 0; i < rec_nbuffers; i++)
    {
      buf_desc.numbytes = rec_buf_size;
      buf_desc.u.pbuffer = &rec_bufs[i];
      ret = ioctl(record_fd, AUDIOIOC_ALLOCBUFFER,
                  (unsigned long)&buf_desc);
      if (ret != sizeof(buf_desc))
        {
          goto err_out;
        }
    }

  for (i = 0; i < play_nbuffers; i++)
    {
      buf_desc.numbytes = play_buf_size;
      buf_desc.u.pbuffer = &play_bufs[i];
      ret = ioctl(play_fd, AUDIOIOC_ALLOCBUFFER,
                  (unsigned long)&buf_desc);
      if (ret != sizeof(buf_desc))
        {
          goto err_out;
        }
    }

  struct dq_queue_s play_avail;
  struct dq_queue_s rec_done;
  dq_init(&play_avail);
  dq_init(&rec_done);

  /* Start playback with silence */

  for (i = 0; i < play_nbuffers; i++)
    {
      memset(play_bufs[i]->samp, 0, play_buf_size);
      play_bufs[i]->nbytes = play_buf_size;
      play_bufs[i]->curbyte = 0;
      play_bufs[i]->flags = AUDIO_APB_PLAY;
      buf_desc.numbytes = play_buf_size;
      buf_desc.u.buffer = play_bufs[i];
      ioctl(play_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
    }

  ioctl(play_fd, AUDIOIOC_START, 0);

  /* Enqueue record buffers and start recording */

  for (i = 0; i < rec_nbuffers; i++)
    {
      rec_bufs[i]->nbytes = rec_buf_size;
      rec_bufs[i]->curbyte = 0;
      rec_bufs[i]->flags = AUDIO_APB_RECORD;
      buf_desc.numbytes = rec_buf_size;
      buf_desc.u.buffer = rec_bufs[i];
      ioctl(record_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
    }

  ioctl(record_fd, AUDIOIOC_START, 0);

  /* Main audio processing loop */

  for (;;)
    {
      ssize_t size;

      size = mq_receive(audio_mq, (char *)&msg, sizeof(msg), &prio);
      if (size != sizeof(msg))
        {
          continue;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE)
        {
          struct ap_buffer_s *apb = (struct ap_buffer_s *)msg.u.ptr;
          bool is_play = false;
          bool is_rec = false;
          int j;

          for (j = 0; j < play_nbuffers; j++)
            {
              if (apb == play_bufs[j]) { is_play = true; break; }
            }

          if (!is_play)
            {
              for (j = 0; j < rec_nbuffers; j++)
                {
                  if (apb == rec_bufs[j]) { is_rec = true; break; }
                }
            }

          if (is_play)
            {
              apb->curbyte = 0;
              dq_addlast(&apb->dq_entry, &play_avail);
            }
          else if (is_rec)
            {
              apb->curbyte = 0;
              dq_addlast(&apb->dq_entry, &rec_done);
            }

          while (dq_count(&rec_done) > 0 && dq_count(&play_avail) > 0)
            {
              struct ap_buffer_s *rec_apb =
                  (struct ap_buffer_s *)dq_remfirst(&rec_done);
              struct ap_buffer_s *play_apb =
                  (struct ap_buffer_s *)dq_remfirst(&play_avail);

              int16_t *rx_data = (int16_t *)rec_apb->samp;
              int16_t *tx_data = (int16_t *)play_apb->samp;
              unsigned int nsamples =
                  rec_apb->nbytes / BYTES_PER_SAMPLE;
              if (nsamples > g_frames_per_buffer)
                {
                  nsamples = g_frames_per_buffer;
                }

              if (g_process_fn != NULL)
                {
                  g_process_fn(tx_data, rx_data, nsamples,
                               g_user_data);
                }
              else
                {
                  memcpy(tx_data, rx_data,
                         nsamples * BYTES_PER_SAMPLE);
                }

              /* Re-enqueue record buffer */

              rec_apb->nbytes = rec_buf_size;
              rec_apb->curbyte = 0;
              rec_apb->flags = AUDIO_APB_RECORD;
              buf_desc.numbytes = rec_buf_size;
              buf_desc.u.buffer = rec_apb;
              ioctl(record_fd, AUDIOIOC_ENQUEUEBUFFER,
                    (unsigned long)&buf_desc);

              /* Enqueue play buffer */

              play_apb->nbytes = nsamples * BYTES_PER_SAMPLE;
              play_apb->curbyte = 0;
              play_apb->flags = AUDIO_APB_PLAY;
              buf_desc.numbytes = play_apb->nbytes;
              buf_desc.u.buffer = play_apb;
              ioctl(play_fd, AUDIOIOC_ENQUEUEBUFFER,
                    (unsigned long)&buf_desc);
            }
        }
    }

err_out:
  if (rec_bufs != NULL)
    {
      for (i = 0; i < rec_nbuffers; i++)
        {
          if (rec_bufs[i] != NULL)
            {
              buf_desc.u.buffer = rec_bufs[i];
              ioctl(record_fd, AUDIOIOC_FREEBUFFER,
                    (unsigned long)&buf_desc);
            }
        }
      OVE_BACKEND_FREE(rec_bufs);
    }

  if (play_bufs != NULL)
    {
      for (i = 0; i < play_nbuffers; i++)
        {
          if (play_bufs[i] != NULL)
            {
              buf_desc.u.buffer = play_bufs[i];
              ioctl(play_fd, AUDIOIOC_FREEBUFFER,
                    (unsigned long)&buf_desc);
            }
        }
      OVE_BACKEND_FREE(play_bufs);
    }

  return NULL;
}

/* ========================================================================= */
/* AUDIO DEVICE SETUP                                                        */
/* ========================================================================= */

static int setup_audio_device(unsigned int sample_rate,
                              unsigned int channels,
                              unsigned int bit_depth)
{
  struct audio_caps_desc_s cap_desc;
  struct mq_attr mq_attr;
  int ret;

  record_fd = open(AUDIO_REC_PATH, O_RDONLY);
  if (record_fd < 0)
    {
      return OVE_ERR_NOT_SUPPORTED;
    }

  play_fd = open(AUDIO_PLAY_PATH, O_WRONLY);
  if (play_fd < 0)
    {
      close(record_fd);
      return OVE_ERR_NOT_SUPPORTED;
    }

  ioctl(record_fd, AUDIOIOC_RESERVE, 0);
  ioctl(play_fd, AUDIOIOC_RESERVE, 0);

  /* Configure recording */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels = channels;
  cap_desc.caps.ac_controls.hw[0] = sample_rate;
  cap_desc.caps.ac_controls.b[2] = bit_depth;

  ret = ioctl(record_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
  if (ret < 0)
    {
      printf("Failed to configure audio input: %d\n", errno);
    }

  /* Configure playback */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels = channels;
  cap_desc.caps.ac_controls.hw[0] = sample_rate;
  cap_desc.caps.ac_controls.b[2] = bit_depth;

  ret = ioctl(play_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
  if (ret < 0)
    {
      printf("Failed to configure audio output: %d\n", errno);
    }

  /* Create message queue */

  mq_attr.mq_maxmsg = g_num_buffers * 4;
  mq_attr.mq_msgsize = sizeof(struct audio_msg_s);
  mq_attr.mq_curmsgs = 0;
  mq_attr.mq_flags = 0;

  audio_mq = mq_open("/" CONFIG_OVE_APP_NAME "_audio", O_RDWR | O_CREAT, 0644, &mq_attr);
  if (audio_mq == (mqd_t)-1)
    {
      close(record_fd);
      close(play_fd);
      return OVE_ERR_NOT_SUPPORTED;
    }

  ioctl(record_fd, AUDIOIOC_REGISTERMQ, (unsigned long)audio_mq);
  ioctl(play_fd, AUDIOIOC_REGISTERMQ, (unsigned long)audio_mq);

  return OVE_OK;
}

/* ========================================================================= */
/* OPS IMPLEMENTATION                                                        */
/* ========================================================================= */

int ove_audio_init(const struct ove_audio_config *cfg,
                            ove_audio_process_fn fn, void *user_data)
{
  if (cfg == NULL || fn == NULL)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  g_process_fn = fn;
  g_user_data = user_data;
  g_frames_per_buffer = cfg->frames_per_buffer;
  g_num_buffers = cfg->num_buffers ? cfg->num_buffers
                                   : DEFAULT_NUM_BUFFERS;
  g_thread_priority = cfg->thread_priority ? cfg->thread_priority
                                           : DEFAULT_AUDIO_PRIORITY;
  g_thread_stack_size = cfg->thread_stack_size ? cfg->thread_stack_size
                                               : DEFAULT_AUDIO_STACK;

  sem_init(&audio_ready_sem, 0, 0);

  return setup_audio_device(cfg->sample_rate, cfg->channels,
                            cfg->bit_depth);
}

int ove_audio_start(void)
{
  pthread_attr_t attr;
  struct sched_param param;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, g_thread_stack_size);
  param.sched_priority = g_thread_priority;
  pthread_attr_setschedparam(&attr, &param);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

  ret = pthread_create(&audio_tid, &attr, audio_thread_fn, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      return OVE_ERR_NO_MEMORY;
    }

  pthread_setname_np(audio_tid, CONFIG_OVE_APP_NAME "_audio");
  sem_post(&audio_ready_sem);
  return OVE_OK;
}

int ove_audio_stop(void)
{
  ioctl(record_fd, AUDIOIOC_STOP, 0);
  ioctl(play_fd, AUDIOIOC_STOP, 0);
  return OVE_OK;
}

void ove_audio_deinit(void)
{
  if (audio_mq != (mqd_t)-1)
    {
      mq_close(audio_mq);
      mq_unlink("/" CONFIG_OVE_APP_NAME "_audio");
    }

  if (record_fd >= 0)
    {
      close(record_fd);
    }

  if (play_fd >= 0)
    {
      close(play_fd);
    }

  sem_destroy(&audio_ready_sem);
}
