/****************************************************************************
 * modules/audio/objects/media_recorder/media_recorder_obj.cpp
 *
 *   Copyright 2018 Sony Semiconductor Solutions Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of Sony Semiconductor Solutions Corporation nor
 *    the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdlib.h>
#include <nuttx/arch.h>
#include <stdlib.h>
#include <arch/chip/cxd56_audio.h>
#include "memutils/common_utils/common_assert.h"
#include "media_recorder_obj.h"
#include "components/encoder/encoder_component.h"
#include "components/filter/filter_component.h"
#include "dsp_driver/include/dsp_drv.h"
#include "wien2_internal_packet.h"
#include "debug/dbg_log.h"

__WIEN2_BEGIN_NAMESPACE
using namespace MemMgrLite;

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pid_t    s_rcd_pid;
static MsgQueId s_self_dtq;
static MsgQueId s_manager_dtq;
static MsgQueId s_apu_dtq;
static PoolId   s_apu_pool_id;
static PoolId   s_in_pool_id;
static PoolId   s_out_pool_id;

static MediaRecorderObjectTask *s_rcd_obj = NULL;

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void capture_comp_done_callback(CaptureDataParam param)
{
  err_t er;

  er = MsgLib::send<CaptureDataParam>(s_self_dtq,
                                      MsgPriNormal,
                                      MSG_AUD_VRC_RST_CAPTURE,
                                      s_self_dtq,
                                      param);
  F_ASSERT(er == ERR_OK);
}

/*--------------------------------------------------------------------------*/
static bool src_done_callback(DspDrvComPrm_t* p_param)
{
  D_ASSERT2(DSP_COM_DATA_TYPE_STRUCT_ADDRESS == p_param->type,
            AssertParamLog(AssertIdTypeUnmatch, p_param->type));

  err_t er;
  SrcFilterCompCmpltParam cmplt;
  Apu::Wien2ApuCmd* packet = reinterpret_cast<Apu::Wien2ApuCmd*>
    (p_param->data.pParam);

  cmplt.event_type = static_cast<Wien2::Apu::ApuEventType>
    (packet->header.event_type);

  switch (packet->header.event_type)
    {
      case Apu::ExecEvent:
        {
          cmplt.exec_src_param.input_buffer  =
            packet->exec_filter_cmd.input_buffer;
          cmplt.exec_src_param.output_buffer =
            packet->exec_filter_cmd.output_buffer;

          MEDIA_RECORDER_VDBG("Src s %d\n",
                              cmplt.exec_src_param.output_buffer.size);

          er = MsgLib::send<SrcFilterCompCmpltParam>(s_self_dtq,
                                                     MsgPriNormal,
                                                     MSG_AUD_VRC_RST_FILTER,
                                                     s_self_dtq,
                                                     cmplt);

          F_ASSERT(er == ERR_OK);
        }
        break;

      case Apu::FlushEvent:
        {
          cmplt.stop_src_param.output_buffer =
            packet->flush_filter_cmd.flush_src_cmd.output_buffer;

          MEDIA_RECORDER_VDBG("FlsSrc s %d\n",
                              cmplt.stop_src_param.output_buffer.size);

          er = MsgLib::send<SrcFilterCompCmpltParam>(s_self_dtq,
                                                     MsgPriNormal,
                                                     MSG_AUD_VRC_RST_FILTER,
                                                     s_self_dtq,
                                                     cmplt);
          F_ASSERT(er == ERR_OK);
        }
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_DSP_ILLEGAL_REPLY);
        return false;
    }
  return true;
}

/*--------------------------------------------------------------------------*/
static bool encoder_done_callback(void* p_response)
{
  err_t er;
  EncCmpltParam cmplt;
  DspDrvComPrm_t* p_param = (DspDrvComPrm_t *)p_response;
  D_ASSERT2(DSP_COM_DATA_TYPE_STRUCT_ADDRESS == p_param->type,
            AssertParamLog(AssertIdTypeUnmatch, p_param->type));

  Apu::Wien2ApuCmd* packet = reinterpret_cast<Apu::Wien2ApuCmd*>
    (p_param->data.pParam);

  cmplt.event_type = static_cast<Wien2::Apu::ApuEventType>
    (packet->header.event_type);

  switch (packet->header.event_type)
    {
      case Apu::ExecEvent:
        {
          cmplt.exec_enc_cmplt.input_buffer  =
            packet->exec_enc_cmd.input_buffer;
          cmplt.exec_enc_cmplt.output_buffer =
            packet->exec_enc_cmd.output_buffer;

          MEDIA_RECORDER_VDBG("Enc s %d\n",
                              cmplt.exec_enc_cmplt.output_buffer.size);

          er = MsgLib::send<EncCmpltParam>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_RST_ENC,
                                           s_self_dtq,
                                           cmplt);
          F_ASSERT(er == ERR_OK);
        }
        break;

      case Apu::FlushEvent:
        {
          cmplt.stop_enc_cmplt.output_buffer =
            packet->flush_enc_cmd.output_buffer;

          MEDIA_RECORDER_VDBG("FlsEnc s %d\n",
                              cmplt.stop_enc_cmplt.output_buffer.size);

          er = MsgLib::send<EncCmpltParam>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_RST_ENC,
                                           s_self_dtq,
                                           cmplt);
          F_ASSERT(er == ERR_OK);
        }
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_DSP_ILLEGAL_REPLY);
        return false;
    }
  return true;
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::loadCodec(AudioCodec codec,
                                            char *path,
                                            int32_t sampling_rate,
                                            uint32_t* dsp_inf)
{
  uint32_t rst = AS_ECODE_OK;
  if ((codec == AudCodecMP3) || (codec == AudCodecOPUS))
    {
      rst = AS_encode_activate(codec,
                               (path) ? path : CONFIG_AUDIOUTILS_DSP_MOUNTPT,
                               s_apu_dtq,
                               s_apu_pool_id,
                               dsp_inf);
      if(rst != AS_ECODE_OK)
        {
          return rst;
        }
    }
  else if (codec == AudCodecLPCM)
    {
      if (sampling_rate != AS_SAMPLINGRATE_48000)
        {
          rst = AS_filter_activate(SRCOnly,
                                   (path) ? path : CONFIG_AUDIOUTILS_DSP_MOUNTPT,
                                   s_apu_dtq,
                                   s_apu_pool_id,
                                   dsp_inf);
          if (rst != AS_ECODE_OK)
            {
              return rst;
            }
        }
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return AS_ECODE_COMMAND_PARAM_CODEC_TYPE;
    }

  m_codec_type = codec;

  return AS_ECODE_OK;
}

/*--------------------------------------------------------------------------*/
bool MediaRecorderObjectTask::unloadCodec(void)
{
  if ((m_codec_type == AudCodecMP3) || (m_codec_type == AudCodecOPUS))
    {
      if (!AS_encode_deactivate())
        {
          return false;
        }
    }
  else if ((m_codec_type == AudCodecLPCM) &&
    (m_sampling_rate != AS_SAMPLINGRATE_48000))
    {
      if(!AS_filter_deactivate(SRCOnly))
        {
          return false;
        }
    }
  else
    {
      /* No need to unload DSP because it is not loaded */
    }

  m_codec_type = InvalidCodecType;

  return true;
}

/*--------------------------------------------------------------------------*/
int AS_MediaRecorderObjEntry(int argc, char *argv[])
{
  MediaRecorderObjectTask::create(s_self_dtq,
                                  s_manager_dtq);
  return 0;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::run(void)
{
  err_t        err_code;
  MsgQueBlock* que;
  MsgPacket*   msg;

  err_code = MsgLib::referMsgQueBlock(m_self_dtq, &que);
  F_ASSERT(err_code == ERR_OK);

  while(1)
    {
      err_code = que->recv(TIME_FOREVER, &msg);
      F_ASSERT(err_code == ERR_OK);

      parse(msg);

      err_code = que->pop();
      F_ASSERT(err_code == ERR_OK);
    }
}

/*--------------------------------------------------------------------------*/
MediaRecorderObjectTask::MsgProc
  MediaRecorderObjectTask::MsgProcTbl[AUD_VRC_MSG_NUM][RecorderStateNum] =
{
  /* Message Type: MSG_AUD_VRC_CMD_ACTIVATE. */

  {                                           /* Recorder status: */
    &MediaRecorderObjectTask::activate,       /*   Inactive.      */
    &MediaRecorderObjectTask::illegal,        /*   Ready.         */
    &MediaRecorderObjectTask::illegal,        /*   Play.          */
    &MediaRecorderObjectTask::illegal,        /*   Stopping.      */
    &MediaRecorderObjectTask::illegal,        /*   Overflow.      */
    &MediaRecorderObjectTask::illegal         /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_CMD_DEACTIVATE. */

  {                                           /* Recorder status: */
    &MediaRecorderObjectTask::illegal,        /*   Inactive.      */
    &MediaRecorderObjectTask::deactivate,     /*   Ready.         */
    &MediaRecorderObjectTask::illegal,        /*   Play.          */
    &MediaRecorderObjectTask::illegal,        /*   Stopping.      */
    &MediaRecorderObjectTask::illegal,        /*   Overflow.      */
    &MediaRecorderObjectTask::illegal         /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_CMD_INIT. */

  {                                           /* Recorder status: */
    &MediaRecorderObjectTask::illegal,        /*   Inactive.      */
    &MediaRecorderObjectTask::init,           /*   Ready.         */
    &MediaRecorderObjectTask::illegal,        /*   Play.          */
    &MediaRecorderObjectTask::illegal,        /*   Stopping.      */
    &MediaRecorderObjectTask::illegal,        /*   Overflow.      */
    &MediaRecorderObjectTask::illegal         /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_CMD_START. */

  {                                           /* Recorder status: */
    &MediaRecorderObjectTask::illegal,        /*   Inactive.      */
    &MediaRecorderObjectTask::startOnReady,   /*   Ready.         */
    &MediaRecorderObjectTask::illegal,        /*   Play.          */
    &MediaRecorderObjectTask::illegal,        /*   Stopping.      */
    &MediaRecorderObjectTask::illegal,        /*   Overflow.      */
    &MediaRecorderObjectTask::illegal         /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_CMD_STOP. */

  {                                           /* Recorder status: */
    &MediaRecorderObjectTask::illegal,        /*   Inactive.      */
    &MediaRecorderObjectTask::illegal,        /*   Ready.         */
    &MediaRecorderObjectTask::stopOnRec,      /*   Play.          */
    &MediaRecorderObjectTask::illegal,        /*   Stopping.      */
    &MediaRecorderObjectTask::stopOnOverflow, /*   Overflow.      */
    &MediaRecorderObjectTask::stopOnWait      /*   WaitStop.      */
  }
};

/*--------------------------------------------------------------------------*/
MediaRecorderObjectTask::MsgProc
  MediaRecorderObjectTask::RstProcTbl[AUD_VRC_RST_MSG_NUM][RecorderStateNum] =
{
  /* Message Type: MSG_AUD_VRC_RST_CAPTURE. */

  {                                                  /* Recorder status: */
    &MediaRecorderObjectTask::illegalCaptureDone,    /*   Inactive.      */
    &MediaRecorderObjectTask::illegalCaptureDone,    /*   Ready.         */
    &MediaRecorderObjectTask::captureDoneOnRec,      /*   Play.          */
    &MediaRecorderObjectTask::captureDoneOnStop,     /*   Stopping.      */
    &MediaRecorderObjectTask::captureDoneOnStop,     /*   Overflow.      */
    &MediaRecorderObjectTask::illegalCaptureDone     /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_RST_FILTER. */

  {                                                  /* Recorder status: */
    &MediaRecorderObjectTask::illegalFilterDone,     /*   Inactive.      */
    &MediaRecorderObjectTask::illegalFilterDone,     /*   Ready.         */
    &MediaRecorderObjectTask::filterDoneOnRec,       /*   Play.          */
    &MediaRecorderObjectTask::filterDoneOnStop,      /*   Stopping.      */
    &MediaRecorderObjectTask::filterDoneOnOverflow,  /*   Overflow.      */
    &MediaRecorderObjectTask::illegalFilterDone      /*   WaitStop.      */
  },

  /* Message Type: MSG_AUD_VRC_RST_ENC. */

  {                                                  /* Recorder status: */
    &MediaRecorderObjectTask::illegalEncDone,        /*   Inactive.      */
    &MediaRecorderObjectTask::illegalEncDone,        /*   Ready.         */
    &MediaRecorderObjectTask::encDoneOnRec,          /*   Play.          */
    &MediaRecorderObjectTask::encDoneOnStop,         /*   Stopping.      */
    &MediaRecorderObjectTask::encDoneOnOverflow,     /*   Overflow.      */
    &MediaRecorderObjectTask::illegalEncDone         /*   WaitStop.      */
  }
};

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::parse(MsgPacket *msg)
{
  uint32_t event;

  if (MSG_IS_REQUEST(msg->getType()) == 0)
    {
      event = MSG_GET_SUBTYPE(msg->getType());
      F_ASSERT((event < AUD_VRC_RST_MSG_NUM));

      (this->*RstProcTbl[event][m_state.get()])(msg);
    }
  else
    {
      event = MSG_GET_SUBTYPE(msg->getType());
      F_ASSERT((event < AUD_VRC_MSG_NUM));

      (this->*MsgProcTbl[event][m_state.get()])(msg);
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::illegal(MsgPacket *msg)
{
  uint msgtype = msg->getType();
  msg->moveParam<RecorderCommand>();

  uint32_t idx = msgtype - MSG_AUD_VRC_CMD_ACTIVATE;

  AsRecorderEvent table[] =
  {
    AsRecorderEventAct,
    AsRecorderEventInit,
    AsRecorderEventStart,
    AsRecorderEventStop,
    AsRecorderEventDeact,
  };

  m_callback(table[idx], AS_ECODE_STATE_VIOLATION, 0);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::activate(MsgPacket *msg)
{
  uint32_t rst;
  AsActivateRecorder act = msg->moveParam<RecorderCommand>().act_param;

  MEDIA_RECORDER_DBG("ACT: indev %d, outdev %d\n",
                     act.param.input_device,
                     act.param.output_device);

  /* Set event callback */

  m_callback = act.cb;

  if (!checkAndSetMemPool())
    {
      m_callback(AsRecorderEventAct,
                 AS_ECODE_CHECK_MEMORY_POOL_ERROR, 0);
      return;
    }

  rst = isValidActivateParam(act.param);
  if (rst != AS_ECODE_OK)
    {
      m_callback(AsRecorderEventAct, rst, 0);
      return;
    }

  switch (m_output_device)
    {
      case AS_SETRECDR_STS_OUTPUTDEVICE_RAM:
        m_p_output_device_handler =
          act.param.output_device_handler;
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        m_callback(AsRecorderEventAct, AS_ECODE_COMMAND_PARAM_OUTPUT_DEVICE, 0);
        return;
    }

  m_state = RecorderStateReady;

  m_callback(AsRecorderEventAct, AS_ECODE_OK, 0);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::deactivate(MsgPacket *msg)
{
  msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("DEACT:\n");

  if (!delInputDeviceHdlr())
    {
      m_callback(AsRecorderEventDeact, AS_ECODE_CLEAR_AUDIO_DATA_PATH_ERROR, 0);
      return;
    }

  if (!unloadCodec())
    {
      m_callback(AsRecorderEventDeact, AS_ECODE_DSP_UNLOAD_ERROR, 0);
      return;
    }

  m_state = RecorderStateInactive;

  m_callback(AsRecorderEventDeact, AS_ECODE_OK, 0);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::init(MsgPacket *msg)
{
  uint32_t rst;
  RecorderCommand cmd = msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("INIT: fs %d, ch num %d, bit len %d, codec %d(%s),"
                     "complexity %d, bitrate %d\n",
                     cmd.init_param.sampling_rate,
                     cmd.init_param.channel_number,
                     cmd.init_param.bit_length,
                     cmd.init_param.codec_type,
                     cmd.init_param.dsp_path,
                     cmd.init_param.computational_complexity,
                     cmd.init_param.bitrate);

  rst = isValidInitParam(cmd);
  if (rst != AS_ECODE_OK)
    {
      m_callback(AsRecorderEventInit, rst, 0);
      return;
    }

  m_channel_num   = cmd.init_param.channel_number;
  m_pcm_bit_width =
    ((cmd.init_param.bit_length == AS_BITLENGTH_16) ?
      AudPcm16Bit : AudPcm24Bit);
  m_pcm_byte_len  = ((m_pcm_bit_width == AudPcm16Bit) ? 2 : 4);
  m_bit_rate      = cmd.init_param.bitrate;
  m_complexity    = cmd.init_param.computational_complexity;
  AudioCodec cmd_codec_type = InvalidCodecType;
  switch (cmd.init_param.codec_type)
    {
      case AS_CODECTYPE_MP3:
        cmd_codec_type = AudCodecMP3;
        break;
      case AS_CODECTYPE_LPCM:
        cmd_codec_type = AudCodecLPCM;
        break;
      case AudCodecOPUS:
        cmd_codec_type = AudCodecOPUS;
        break;
      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        m_callback(AsRecorderEventInit, AS_ECODE_COMMAND_PARAM_CODEC_TYPE, 0);
        return;
    }
  if (m_codec_type != cmd_codec_type)
    {
      if (!unloadCodec())
        {
          m_callback(AsRecorderEventInit, AS_ECODE_DSP_UNLOAD_ERROR, 0);
          return;
        }
      if (!delInputDeviceHdlr())
        {
          m_callback(AsRecorderEventInit, AS_ECODE_CLEAR_AUDIO_DATA_PATH_ERROR, 0);
          return;
        }

      uint32_t dsp_inf = 0;
      rst = loadCodec(cmd_codec_type,
                      cmd.init_param.dsp_path,
                      cmd.init_param.sampling_rate,
                      &dsp_inf);
      if (rst != AS_ECODE_OK)
        {
          m_callback(AsRecorderEventInit, rst, dsp_inf);
          return;
        }

      if (!getInputDeviceHdlr())
        {
          m_callback(AsRecorderEventInit, AS_ECODE_SET_AUDIO_DATA_PATH_ERROR, 0);
          return;
        }
    }
  else
    {
      if (m_codec_type == AudCodecLPCM &&
          (m_sampling_rate == AS_SAMPLINGRATE_16000 &&
           cmd.init_param.sampling_rate == AS_SAMPLINGRATE_48000))
        {
          if (!unloadCodec())
            {
              m_callback(AsRecorderEventInit, AS_ECODE_DSP_UNLOAD_ERROR, 0);
              return;
            }
          m_codec_type = cmd_codec_type;
        }
      if (!delInputDeviceHdlr())
        {
          m_callback(AsRecorderEventInit, AS_ECODE_CLEAR_AUDIO_DATA_PATH_ERROR, 0);
          return;
        }

      if (m_codec_type == AudCodecLPCM &&
          (m_sampling_rate == AS_SAMPLINGRATE_48000 &&
            cmd.init_param.sampling_rate ==
              AS_SAMPLINGRATE_16000))
        {
          uint32_t dsp_inf = 0;
          rst = loadCodec(cmd_codec_type,
                          cmd.init_param.dsp_path,
                          cmd.init_param.sampling_rate,
                          &dsp_inf);
          if (rst != AS_ECODE_OK)
            {
              m_callback(AsRecorderEventInit, rst, dsp_inf);
              return;
            }
        }
      if (!getInputDeviceHdlr())
        {
          m_callback(AsRecorderEventInit, AS_ECODE_SET_AUDIO_DATA_PATH_ERROR, 0);
          return;
        }
    }
  m_sampling_rate = cmd.init_param.sampling_rate;

  m_callback(AsRecorderEventInit, AS_ECODE_OK, 0);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::startOnReady(MsgPacket *msg)
{
  msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("START:\n");

  InitAudioRecSinkParam_s init_sink;
  init_sink.fs            = m_sampling_rate;
  init_sink.ch            = m_channel_num;
  init_sink.byte_length   = m_pcm_byte_len;
  init_sink.codec_type    = m_codec_type;
  init_sink.output_device = m_output_device;
  if (m_output_device == AS_SETRECDR_STS_OUTPUTDEVICE_RAM)
    {
      init_sink.init_audio_ram_sink.output_device_hdlr =
        *m_p_output_device_handler;
    }
  m_rec_sink.init(init_sink);

  CaptureComponentParam cap_comp_param;
  bool result = true;
  uint32_t apu_result = AS_ECODE_OK;
  uint32_t dsp_inf = 0;

  cap_comp_param.init_param.capture_ch_num    = m_channel_num;
  cap_comp_param.init_param.capture_bit_width = m_pcm_bit_width;
  cap_comp_param.init_param.callback          = capture_comp_done_callback;
  cap_comp_param.handle                       = m_capture_from_mic_hdlr;
  if (!AS_init_capture(&cap_comp_param))
    {
      m_callback(AsRecorderEventStart, AS_ECODE_DMAC_INITIALIZE_ERROR, 0);
      return;
    }

  if (m_codec_type == AudCodecLPCM)
    {
      FilterComponentParam filter_param;
      filter_param.filter_type = Apu::SRC;
      filter_param.init_src_param.sample_num = getPcmCaptureSample();
      cxd56_audio_clkmode_t clock_mode = cxd56_audio_get_clkmode();
      if (CXD56_AUDIO_CLKMODE_HIRES == clock_mode)
        {
          filter_param.init_src_param.input_sampling_rate =
            AS_SAMPLINGRATE_192000;
        }
      else
        {
          filter_param.init_src_param.input_sampling_rate =
            AS_SAMPLINGRATE_48000;
        }
      filter_param.init_src_param.output_sampling_rate   = m_sampling_rate;
      filter_param.init_src_param.channel_num            = m_channel_num;
      filter_param.init_src_param.input_pcm_byte_length  = m_pcm_byte_len;
      filter_param.init_src_param.output_pcm_byte_length = m_pcm_byte_len;
      filter_param.callback                              = src_done_callback;
      if (m_sampling_rate != AS_SAMPLINGRATE_48000)
        {
          apu_result = AS_filter_init(filter_param, &dsp_inf);
          result = AS_filter_recv_done();
          if (!result)
            {
              D_ASSERT(0);
              m_callback(AsRecorderEventStart, AS_ECODE_QUEUE_OPERATION_ERROR, 0);
              return;
            }
          if (apu_result != AS_ECODE_OK)
            {
              m_callback(AsRecorderEventStart, apu_result, dsp_inf);
              return;
            }
        }
    }
  else if ((m_codec_type == AudCodecMP3) || (m_codec_type == AudCodecOPUS))
    {
      InitEncParam enc_param;
      enc_param.codec_type           = m_codec_type;
      enc_param.input_sampling_rate  = AS_SAMPLINGRATE_48000;
      enc_param.output_sampling_rate = m_sampling_rate;
      enc_param.bit_width            = m_pcm_bit_width;
      enc_param.channel_num          = m_channel_num;
      enc_param.callback             = encoder_done_callback;
      enc_param.bit_rate             = m_bit_rate;
      if (m_codec_type == AudCodecOPUS)
        {
          enc_param.complexity = m_complexity;
        }
      apu_result = AS_encode_init(&enc_param, &dsp_inf);
      result = AS_encode_recv_done();
      if (!result)
        {
          D_ASSERT(0);
          m_callback(AsRecorderEventStart, AS_ECODE_QUEUE_OPERATION_ERROR, 0);
          return;
        }
      if (apu_result != AS_ECODE_OK)
        {
          m_callback(AsRecorderEventStart, apu_result, dsp_inf);
          return;
        }
      }

  m_output_buf_mh_que.clear();

  if (startCapture())
    {
      m_state = RecorderStateRecording;
      m_fifo_overflow = false;
      m_callback(AsRecorderEventStart, AS_ECODE_OK, 0);
      return;
    }
  else
    {
      m_callback(AsRecorderEventStart, AS_ECODE_DMAC_READ_ERROR, 0);
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::stopOnRec(MsgPacket *msg)
{
  CaptureComponentParam cap_comp_param;
  msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("STOP:\n");

  if (!m_external_cmd_que.push(AsRecorderEventStop))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_PUSH_ERROR);
      m_callback(AsRecorderEventStop, AS_ECODE_QUEUE_OPERATION_ERROR, 0);
      return;
    }

  /* Stop DMA transfer. */

  cap_comp_param.handle          = m_capture_from_mic_hdlr;
  cap_comp_param.stop_param.mode = AS_DMASTOPMODE_NORMAL;

  AS_stop_capture(&cap_comp_param);

  m_state = RecorderStateStopping;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::stopOnOverflow(MsgPacket *msg)
{
  msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("STOP:\n");

  if (!m_external_cmd_que.push(AsRecorderEventStop))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_PUSH_ERROR);
      m_callback(AsRecorderEventStop, AS_ECODE_QUEUE_OPERATION_ERROR, 0);
    }

  m_state = RecorderStateStopping;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::stopOnWait(MsgPacket *msg)
{
  msg->moveParam<RecorderCommand>();

  MEDIA_RECORDER_DBG("STOP:\n");

  m_callback(AsRecorderEventStop, AS_ECODE_OK, 0);

  m_state = RecorderStateReady;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::illegalFilterDone(MsgPacket *msg)
{
  msg->moveParam<SrcFilterCompCmpltParam>();
  MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_ILLEGAL_REQUEST);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::filterDoneOnRec(MsgPacket *msg)
{
  SrcFilterCompCmpltParam filter_result =
    msg->moveParam<SrcFilterCompCmpltParam>();
  if (m_sampling_rate != AS_SAMPLINGRATE_48000)
    {
      AS_filter_recv_done();
    }

  freeCnvInBuf();

  writeToDataSinker(m_output_buf_mh_que.top(),
                   filter_result.exec_src_param.output_buffer.size);
  freeOutputBuf();
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::filterDoneOnStop(MsgPacket *msg)
{
  SrcFilterCompCmpltParam filter_result =
    msg->moveParam<SrcFilterCompCmpltParam>();
  if (m_sampling_rate != AS_SAMPLINGRATE_48000)
    {
      AS_filter_recv_done();
    }

  if (filter_result.event_type == Apu::ExecEvent)
    {
      if (m_codec_type == AudCodecLPCM)
        {
          freeCnvInBuf();

          writeToDataSinker(m_output_buf_mh_que.top(),
                           filter_result.exec_src_param.output_buffer.size);
          freeOutputBuf();
        }
    }
  else if (filter_result.event_type == Apu::FlushEvent)
    {
      if (filter_result.stop_src_param.output_buffer.size > 0)
        {
          writeToDataSinker(m_output_buf_mh_que.top(),
                           filter_result.stop_src_param.output_buffer.size);
        }
      freeOutputBuf();

      m_rec_sink.finalize();

      if (m_external_cmd_que.empty())
        {
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_MISSING_ERROR);
          return;
        }
      AsRecorderEvent ext_cmd = m_external_cmd_que.top();
      if (!m_external_cmd_que.pop())
        {
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_POP_ERROR);
        }

      m_callback(ext_cmd, AS_ECODE_OK, 0);
      m_state = RecorderStateReady;
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return;
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::filterDoneOnOverflow(MsgPacket *msg)
{
  SrcFilterCompCmpltParam filter_result =
    msg->moveParam<SrcFilterCompCmpltParam>();
  if (m_sampling_rate != AS_SAMPLINGRATE_48000)
    {
      AS_filter_recv_done();
    }

  if (filter_result.event_type == Apu::ExecEvent)
    {
      if (m_codec_type == AudCodecLPCM)
        {
          freeCnvInBuf();

          writeToDataSinker(m_output_buf_mh_que.top(),
                           filter_result.exec_src_param.output_buffer.size);

          freeOutputBuf();
        }
    }
  else if (filter_result.event_type == Apu::FlushEvent)
    {
      if (filter_result.stop_src_param.output_buffer.size > 0)
        {
          writeToDataSinker(m_output_buf_mh_que.top(),
                           filter_result.stop_src_param.output_buffer.size);
        }

      freeOutputBuf();

      m_rec_sink.finalize();

      m_state = RecorderStateWaitStop;
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return;
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::illegalEncDone(MsgPacket *msg)
{
  msg->moveParam<EncCmpltParam>();
  MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_ILLEGAL_REQUEST);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::encDoneOnRec(MsgPacket *msg)
{
  EncCmpltParam enc_result = msg->moveParam<EncCmpltParam>();
  AS_encode_recv_done();

  freeCnvInBuf();

  writeToDataSinker(m_output_buf_mh_que.top(),
                   enc_result.exec_enc_cmplt.output_buffer.size);
  freeOutputBuf();
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::encDoneOnStop(MsgPacket *msg)
{
  EncCmpltParam enc_result = msg->moveParam<EncCmpltParam>();
  AS_encode_recv_done();

  if (enc_result.event_type == Apu::ExecEvent)
    {
      freeCnvInBuf();

      writeToDataSinker(m_output_buf_mh_que.top(),
                       enc_result.exec_enc_cmplt.output_buffer.size);
      freeOutputBuf();
    }
  else if (enc_result.event_type == Apu::FlushEvent)
    {
      if (enc_result.stop_enc_cmplt.output_buffer.size > 0)
        {
          writeToDataSinker(m_output_buf_mh_que.top(),
                           enc_result.stop_enc_cmplt.output_buffer.size);
        }
      freeOutputBuf();

      m_rec_sink.finalize();

      if (m_external_cmd_que.empty())
        {
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_MISSING_ERROR);
          return;
        }
      AsRecorderEvent ext_cmd = m_external_cmd_que.top();
      if (!m_external_cmd_que.pop())
        {
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_POP_ERROR);
        }

      m_callback(ext_cmd, AS_ECODE_OK, 0);
      m_state = RecorderStateReady;
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return;
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::encDoneOnOverflow(MsgPacket *msg)
{
  EncCmpltParam enc_result = msg->moveParam<EncCmpltParam>();
  AS_encode_recv_done();

  if (enc_result.event_type == Apu::ExecEvent)
    {
      freeCnvInBuf();

      writeToDataSinker(m_output_buf_mh_que.top(),
                       enc_result.exec_enc_cmplt.output_buffer.size);

      freeOutputBuf();
    }
  else if (enc_result.event_type == Apu::FlushEvent)
    {
      if (enc_result.stop_enc_cmplt.output_buffer.size > 0)
        {
          writeToDataSinker(m_output_buf_mh_que.top(),
                           enc_result.stop_enc_cmplt.output_buffer.size);
        }

      freeOutputBuf();

      m_rec_sink.finalize();

      m_state = RecorderStateWaitStop;
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return;
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::illegalCaptureDone(MsgPacket *msg)
{
  msg->moveParam<CaptureDataParam>();
  MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_ILLEGAL_REQUEST);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::captureDoneOnRec(MsgPacket *msg)
{
  CaptureComponentParam cap_comp_param;
  CaptureDataParam capture_result = msg->moveParam<CaptureDataParam>();

  /* Request next capture */

  cap_comp_param.handle                = m_capture_from_mic_hdlr;
  cap_comp_param.exec_param.pcm_sample = getPcmCaptureSample();
  AS_exec_capture(&cap_comp_param);

  /* Transfer Mic-in pcm data. */

  execEnc(capture_result.buf.cap_mh,
          capture_result.buf.sample *
          m_pcm_byte_len * m_channel_num);
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::captureDoneOnStop(MsgPacket *msg)
{
  CaptureDataParam capture_result = msg->moveParam<CaptureDataParam>();

  /* Transfer Mic-in pcm data. */

  execEnc(capture_result.buf.cap_mh,
          capture_result.buf.sample *
          m_pcm_byte_len * m_channel_num);

  /* Check end of capture */

  if (capture_result.end_flag)
    {
      stopEnc();
    }
}

/*--------------------------------------------------------------------------*/
bool MediaRecorderObjectTask::startCapture()
{
  bool result = true;

  /* Note:
   * Always prior introduce 3 commands in according to
   * the specification of Baseband.
   */

  for (int i = 0; i < CAPTURE_DELAY_STAGE_NUM; i++)
    {
      CaptureComponentParam cap_comp_param;
      cap_comp_param.handle                = m_capture_from_mic_hdlr;
      cap_comp_param.exec_param.pcm_sample = getPcmCaptureSample();

      result = AS_exec_capture(&cap_comp_param);
      if (!result)
        {
          break;
        }
    }

  return result;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::execEnc(MemMgrLite::MemHandle mh, uint32_t pcm_size)
{
  if (m_codec_type == AudCodecLPCM)
    {
      if (m_sampling_rate != AS_SAMPLINGRATE_48000)
        {
          FilterComponentParam param;
          param.filter_type = Apu::SRC;
          param.callback = src_done_callback;

          param.exec_src_param.input_buffer.p_buffer  =
            reinterpret_cast<unsigned long *>(mh.getPa());
          param.exec_src_param.input_buffer.size      = pcm_size;
          param.exec_src_param.output_buffer.p_buffer =
            reinterpret_cast<unsigned long *>(getOutputBufAddr());
          param.exec_src_param.output_buffer.size     =
            m_max_output_pcm_size;
          AS_filter_exec(param);

          m_cnv_in_buf_mh_que.push(mh);
        }
      else
        {
          err_t er;
          SrcFilterCompCmpltParam cmplt;
          cmplt.event_type = Apu::ExecEvent;
          cmplt.exec_src_param.input_buffer.p_buffer  =
            reinterpret_cast<unsigned long *>(mh.getPa());
          cmplt.exec_src_param.input_buffer.size      = pcm_size;
          cmplt.exec_src_param.output_buffer.p_buffer =
            reinterpret_cast<unsigned long *>(getOutputBufAddr());
          cmplt.exec_src_param.output_buffer.size     = pcm_size;
          memcpy(cmplt.exec_src_param.output_buffer.p_buffer,
                 cmplt.exec_src_param.input_buffer.p_buffer,
                 pcm_size);

          m_cnv_in_buf_mh_que.push(mh);

          er = MsgLib::send<SrcFilterCompCmpltParam>(s_self_dtq,
                                                     MsgPriNormal,
                                                     MSG_AUD_VRC_RST_FILTER,
                                                     NULL,
                                                     cmplt);
          F_ASSERT(er == ERR_OK);
        }
    }
  else if ((m_codec_type == AudCodecMP3) || (m_codec_type == AudCodecOPUS))
    {
      ExecEncParam param;
      param.input_buffer.p_buffer  =
        reinterpret_cast<unsigned long *>(mh.getPa());
      param.input_buffer.size      = pcm_size;
      param.output_buffer.p_buffer =
        reinterpret_cast<unsigned long *>(getOutputBufAddr());
      param.output_buffer.size     = m_max_output_pcm_size;
      AS_encode_exec(&param);

      m_cnv_in_buf_mh_que.push(mh);
    }
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::stopEnc(void)
{
  if (m_codec_type == AudCodecLPCM)
    {
      if (m_sampling_rate != AS_SAMPLINGRATE_48000)
        {
          FilterComponentParam param;
          param.filter_type = Apu::SRC;
          param.stop_src_param.output_buffer.p_buffer =
            reinterpret_cast<unsigned long *>(getOutputBufAddr());
          param.stop_src_param.output_buffer.size = m_max_output_pcm_size;
          AS_filter_stop(param);
        }
      else
        {
          err_t er;
          SrcFilterCompCmpltParam cmplt;
          cmplt.event_type = Apu::FlushEvent;
          cmplt.stop_src_param.output_buffer.p_buffer =
            reinterpret_cast<unsigned long *>(getOutputBufAddr());
          cmplt.stop_src_param.output_buffer.size = 0;
          er = MsgLib::send<SrcFilterCompCmpltParam>(s_self_dtq,
                                                     MsgPriNormal,
                                                     MSG_AUD_VRC_RST_FILTER,
                                                     NULL,
                                                     cmplt);
          F_ASSERT(er == ERR_OK);
        }
    }
  else if ((m_codec_type == AudCodecMP3) || (m_codec_type == AudCodecOPUS))
    {
      StopEncParam param;
      param.output_buffer.p_buffer =
        reinterpret_cast<unsigned long *>(getOutputBufAddr());
      param.output_buffer.size = m_max_output_pcm_size;
      AS_encode_stop(&param);
    }
}

/*--------------------------------------------------------------------------*/
void* MediaRecorderObjectTask::getOutputBufAddr()
{
  MemMgrLite::MemHandle mh;
  if (mh.allocSeg(s_out_pool_id, m_max_output_pcm_size) != ERR_OK)
    {
      MEDIA_RECORDER_WARN(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return NULL;
    }

  if (!m_output_buf_mh_que.push(mh))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_QUEUE_PUSH_ERROR);
      return NULL;
    }
  return mh.getPa();
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::isValidActivateParam(
  const AsActivateRecorderParam &param)
{
  switch (param.input_device)
    {
      case AS_SETRECDR_STS_INPUTDEVICE_MIC_A:
        m_input_device = CaptureDeviceAnalogMic;
        break;

      case AS_SETRECDR_STS_INPUTDEVICE_MIC_D:
        m_input_device = CaptureDeviceDigitalMic;
        break;

      case AS_SETRECDR_STS_INPUTDEVICE_I2S_IN:
        m_input_device = CaptureDeviceI2S;
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_INPUT_DEVICE;
    }

  switch (param.output_device)
    {
      case AS_SETRECDR_STS_OUTPUTDEVICE_RAM:
        m_output_device = AS_SETRECDR_STS_OUTPUTDEVICE_RAM;
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_OUTPUT_DEVICE;
    }
  return AS_ECODE_OK;
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::isValidInitParam(
  const RecorderCommand& cmd)
{
  uint32_t rst = AS_ECODE_OK;
  switch(cmd.init_param.codec_type)
    {
      case AS_CODECTYPE_MP3:
        rst = isValidInitParamMP3(cmd);
        break;

      case AS_CODECTYPE_LPCM:
        rst = isValidInitParamLPCM(cmd);
        break;

      case AS_CODECTYPE_OPUS:
        rst = isValidInitParamOPUS(cmd);
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_CODEC_TYPE;
    }
  return rst;
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::isValidInitParamMP3(
  const RecorderCommand& cmd)
{
  switch(cmd.init_param.channel_number)
    {
      case AS_CHANNEL_MONO:
        if (m_input_device == CaptureDeviceI2S)
          {
            MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
            return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
          }
        break;

      case AS_CHANNEL_STEREO:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
    }
  switch(cmd.init_param.bit_length)
    {
      case AS_BITLENGTH_16:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_BIT_LENGTH;
    }
  switch(cmd.init_param.sampling_rate)
    {
      case AS_SAMPLINGRATE_16000:
        {
          switch(cmd.init_param.bitrate)
            {
              case AS_BITRATE_8000:
                if (cmd.init_param.channel_number ==
                    AS_CHANNEL_STEREO)
                  {
                    MEDIA_RECORDER_ERR(
                      AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
                    return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
                  }
                break;

              case AS_BITRATE_16000:
              case AS_BITRATE_24000:
              case AS_BITRATE_32000:
              case AS_BITRATE_40000:
              case AS_BITRATE_48000:
              case AS_BITRATE_56000:
              case AS_BITRATE_64000:
              case AS_BITRATE_80000:
              case AS_BITRATE_96000:
              case AS_BITRATE_112000:
              case AS_BITRATE_128000:
              case AS_BITRATE_144000:
              case AS_BITRATE_160000:
                break;

              default:
                MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
                return AS_ECODE_COMMAND_PARAM_BIT_RATE;
            }
        }
        break;

      case AS_SAMPLINGRATE_48000:
        {
          switch(cmd.init_param.bitrate)
            {
              case AS_BITRATE_32000:
              case AS_BITRATE_40000:
              case AS_BITRATE_48000:
              case AS_BITRATE_56000:
              case AS_BITRATE_64000:
              case AS_BITRATE_80000:
              case AS_BITRATE_96000:
              case AS_BITRATE_112000:
              case AS_BITRATE_128000:
              case AS_BITRATE_160000:
              case AS_BITRATE_192000:
              case AS_BITRATE_224000:
              case AS_BITRATE_256000:
              case AS_BITRATE_320000:
                break;

              default:
                MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
                return AS_ECODE_COMMAND_PARAM_BIT_RATE;
            }
        }
        break;
      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_SAMPLING_RATE;
    }
  return AS_ECODE_OK;
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::isValidInitParamLPCM(
  const RecorderCommand& cmd)
{
  switch(cmd.init_param.channel_number)
    {
      case AS_CHANNEL_MONO:
        if (m_input_device == CaptureDeviceI2S)
          {
            MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
            return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
          }
          break;

      case AS_CHANNEL_STEREO:
          break;
      case AS_CHANNEL_4CH:
        if (m_input_device == CaptureDeviceI2S)
          {
            MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
            return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
          }
          break;
      case AS_CHANNEL_6CH:
      case AS_CHANNEL_8CH:
        if (m_input_device != CaptureDeviceDigitalMic)
          {
            MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
            return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
          }
          break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
    }
  switch(cmd.init_param.bit_length)
    {
      case AS_BITLENGTH_16:
        break;

      case AS_BITLENGTH_24:
        {
          cxd56_audio_clkmode_t clock_mode = cxd56_audio_get_clkmode();
          if (CXD56_AUDIO_CLKMODE_HIRES == clock_mode)
            {
              break;
            }
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
          return AS_ECODE_COMMAND_PARAM_BIT_LENGTH;
        }
      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_BIT_LENGTH;
    }
  switch(cmd.init_param.sampling_rate)
    {
      case AS_SAMPLINGRATE_16000:
      case AS_SAMPLINGRATE_48000:
        break;

      case AS_SAMPLINGRATE_192000:
        {
          cxd56_audio_clkmode_t clock_mode = cxd56_audio_get_clkmode();
          if (CXD56_AUDIO_CLKMODE_HIRES == clock_mode)
            {
              break;
            }
          MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
          return AS_ECODE_COMMAND_PARAM_SAMPLING_RATE;
        }
      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_SAMPLING_RATE;
    }
  return AS_ECODE_OK;
}

/*--------------------------------------------------------------------------*/
uint32_t MediaRecorderObjectTask::isValidInitParamOPUS(
  const RecorderCommand& cmd)
{
  switch(cmd.init_param.channel_number)
    {
      case AS_CHANNEL_MONO:
        if (m_input_device == CaptureDeviceI2S)
          {
            MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
            return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
          }
        break;

      case AS_CHANNEL_STEREO:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_CHANNEL_NUMBER;
    }
  switch(cmd.init_param.bit_length)
    {
      case AS_BITLENGTH_16:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_BIT_LENGTH;
    }
  switch(cmd.init_param.sampling_rate)
    {
      case AS_SAMPLINGRATE_8000:
      case AS_SAMPLINGRATE_16000:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_SAMPLING_RATE;
    }
  switch(cmd.init_param.bitrate)
    {
      case AS_BITRATE_8000:
      case AS_BITRATE_16000:
        break;

      default:
        MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
        return AS_ECODE_COMMAND_PARAM_BIT_RATE;
    }
  if (cmd.init_param.computational_complexity >
      AS_INITREC_COMPLEXITY_10)
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_UNEXPECTED_PARAM);
      return AS_ECODE_COMMAND_PARAM_COMPLEXITY;
    }
  return AS_ECODE_OK;
}

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::writeToDataSinker(
  const MemMgrLite::MemHandle& mh,
  uint32_t byte_size)
{
  if (!m_fifo_overflow)
    {
      bool rst = false;
      AudioRecSinkData_s sink_data;
      sink_data.mh = mh;
      sink_data.byte_size = byte_size;

      rst = m_rec_sink.write(sink_data);
      if (!rst)
        {
          CaptureComponentParam cap_comp_param;
          cap_comp_param.handle          = m_capture_from_mic_hdlr;
          cap_comp_param.stop_param.mode = AS_DMASTOPMODE_NORMAL;

          AS_stop_capture(&cap_comp_param);

          m_state = RecorderStateOverflow;
          m_fifo_overflow = true;
        }
    }
}

/*--------------------------------------------------------------------------*/
bool MediaRecorderObjectTask::getInputDeviceHdlr(void)
{
  if (m_capture_from_mic_hdlr != MAX_CAPTURE_COMP_INSTANCE_NUM)
    {
      return false;
    }
  if (m_input_device != CaptureDeviceI2S)
    {
      if (!AS_get_capture_comp_handler(&m_capture_from_mic_hdlr,
                                       m_input_device,
                                       s_in_pool_id))
        {
          return false;
        }
    }
  else
    {
      if (!AS_get_capture_comp_handler(&m_capture_from_mic_hdlr,
                                       m_input_device,
                                       s_in_pool_id))
        {
          return false;
        }
    }
  return true;
}

/*--------------------------------------------------------------------------*/
bool MediaRecorderObjectTask::delInputDeviceHdlr(void)
{
  if (m_capture_from_mic_hdlr != MAX_CAPTURE_COMP_INSTANCE_NUM)
    {
      if(!AS_release_capture_comp_handler(m_capture_from_mic_hdlr))
        {
          return false;
        }
      m_capture_from_mic_hdlr = MAX_CAPTURE_COMP_INSTANCE_NUM;
    }
  return true;
}

/*--------------------------------------------------------------------------*/
bool MediaRecorderObjectTask::checkAndSetMemPool(void)
{
  if (!MemMgrLite::Manager::isPoolAvailable(s_in_pool_id))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  m_max_capture_pcm_size = (MemMgrLite::Manager::getPoolSize(s_in_pool_id)) /
    (MemMgrLite::Manager::getPoolNumSegs(s_in_pool_id));

  if (!MemMgrLite::Manager::isPoolAvailable(s_out_pool_id))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  m_max_output_pcm_size = (MemMgrLite::Manager::getPoolSize(s_out_pool_id)) /
    (MemMgrLite::Manager::getPoolNumSegs(s_out_pool_id));

  if (!MemMgrLite::Manager::isPoolAvailable(s_apu_pool_id))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  if ((int)(sizeof(Apu::Wien2ApuCmd)) >
      (MemMgrLite::Manager::getPoolSize(s_apu_pool_id))/
      (MemMgrLite::Manager::getPoolNumSegs(s_apu_pool_id)))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  if (!MemMgrLite::Manager::isPoolAvailable(s_apu_pool_id))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  if ((int)(sizeof(Apu::Wien2ApuCmd)) >
      (MemMgrLite::Manager::getPoolSize(s_apu_pool_id))/
      (MemMgrLite::Manager::getPoolNumSegs(s_apu_pool_id)))
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_MEMHANDLE_ALLOC_ERROR);
      return false;
    }
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

extern "C"
{
/*--------------------------------------------------------------------------*/
bool AS_CreateMediaRecorder(FAR AsCreateRecorderParam_t *param)
{
  /* Parameter check */

  if (param == NULL)
    {
      return false;
    }

  /* Create */

  s_self_dtq      = param->msgq_id.recorder;
  s_manager_dtq   = param->msgq_id.mng;
  s_apu_dtq       = param->msgq_id.dsp;
  s_in_pool_id    = param->pool_id.input;
  s_out_pool_id   = param->pool_id.output;
  s_apu_pool_id   = param->pool_id.dsp;

  s_rcd_pid = task_create("REC_OBJ",
                          150, 1024 * 2,
                          AS_MediaRecorderObjEntry,
                          NULL);
  if (s_rcd_pid < 0)
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_TASK_CREATE_ERROR);
      return false;
    }

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_ActivateMediaRecorder(FAR AsActivateRecorder *actparam)
{
  /* Parameter check */

  if (actparam == NULL)
    {
      return false;
    }

  /* Activate */

  RecorderCommand cmd;

  cmd.act_param = *actparam;

  err_t er = MsgLib::send<RecorderCommand>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_CMD_ACTIVATE,
                                           s_self_dtq,
                                           cmd);
  F_ASSERT(er == ERR_OK);

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_InitMediaRecorder(FAR AsInitRecorderParam *initparam)
{
  /* Parameter check */

  if (initparam == NULL)
    {
      return false;
    }

  /* Init */

  RecorderCommand cmd;

  cmd.init_param = *initparam;

  err_t er = MsgLib::send<RecorderCommand>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_CMD_INIT,
                                           s_self_dtq,
                                           cmd);
  F_ASSERT(er == ERR_OK);

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_StartMediaRecorder(void)
{
  RecorderCommand cmd;

  err_t er = MsgLib::send<RecorderCommand>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_CMD_START,
                                           s_self_dtq,
                                           cmd);
  F_ASSERT(er == ERR_OK);

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_StopMediaRecorder(void)
{
  RecorderCommand cmd;

  err_t er = MsgLib::send<RecorderCommand>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_CMD_STOP,
                                           s_self_dtq,
                                           cmd);
  F_ASSERT(er == ERR_OK);

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_DeactivateMediaRecorder(void)
{
  RecorderCommand cmd;

  err_t er = MsgLib::send<RecorderCommand>(s_self_dtq,
                                           MsgPriNormal,
                                           MSG_AUD_VRC_CMD_DEACTIVATE,
                                           s_self_dtq,
                                           cmd);
  F_ASSERT(er == ERR_OK);

  return true;
}

/*--------------------------------------------------------------------------*/
bool AS_DeleteMediaRecorder(void)
{
  if (s_rcd_obj == NULL)
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_TASK_CREATE_ERROR);
      return false;
    }

  task_delete(s_rcd_pid);
  delete s_rcd_obj;
  s_rcd_obj = NULL;
  return true;
}
} /* extern "C" */

/*--------------------------------------------------------------------------*/
void MediaRecorderObjectTask::create(MsgQueId self_dtq,
                                     MsgQueId manager_dtq)
{
  if(s_rcd_obj == NULL)
    {
      s_rcd_obj = new MediaRecorderObjectTask(self_dtq,
                                              manager_dtq);
      s_rcd_obj->run();
    }
  else
    {
      MEDIA_RECORDER_ERR(AS_ATTENTION_SUB_CODE_RESOURCE_ERROR);
      return;
    }
}

__WIEN2_END_NAMESPACE
