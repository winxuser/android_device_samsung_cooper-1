/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2012 Zhibin Wu, Simon Davie, Nico Kaiser
 * Copyright (C) 2012 QiSS ME Project Team
 * Copyright (C) 2012 Twisted, Sean Neeley
 * Copyright (C) 2012 GalaxyICS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "CameraHAL"

#define MAX_CAMERAS_SUPPORTED 2
#define GRALLOC_USAGE_PMEM_PRIVATE_ADSP GRALLOC_USAGE_PRIVATE_0

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <linux/ioctl.h>
#include <linux/msm_mdp.h>
#include <cutils/log.h>
//#include <ui/Overlay.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <camera/CameraParameters.h>
#include <hardware/camera.h>
#include <binder/IMemory.h>
#include "CameraHardwareInterface.h"
#include <cutils/properties.h>
#include <utils/Errors.h>
#include <gralloc_priv.h>

#define NO_ERROR 0

using android::sp;
//using android::Overlay;
using android::String8;
using android::IMemory;
using android::IMemoryHeap;
using android::CameraParameters;

using android::CameraInfo;
using android::HAL_getCameraInfo;
using android::HAL_getNumberOfCameras;
using android::HAL_openCameraHardware;
using android::CameraHardwareInterface;

//static android::Mutex gCameraDeviceLock;

static int camera_device_open(const hw_module_t* module, const char* name, hw_device_t** device);
static int camera_device_close(hw_device_t* device);
static int camera_get_number_of_cameras(void);
static int camera_get_camera_info(int camera_id, struct camera_info *info);

static struct hw_module_methods_t camera_module_methods = {
	open: camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
	common: {
		tag: HARDWARE_MODULE_TAG,
		version_major: 1,
		version_minor: 0,
		id: CAMERA_HARDWARE_MODULE_ID,
		name: "GalaxyICS Camera HAL",
		author: "Marcin Chojnacki & Pavel Kirpichyov",
		methods: &camera_module_methods,
		dso: NULL, /* remove compilation warnings */
		reserved: {0}, /* remove compilation warnings */
	},
	get_number_of_cameras: camera_get_number_of_cameras,
	get_camera_info: camera_get_camera_info,
};

static struct {
    int type;
    const char *text;
} msg_map[] = {
    {0x0001, "CAMERA_MSG_ERROR"},
    {0x0002, "CAMERA_MSG_SHUTTER"},
    {0x0004, "CAMERA_MSG_FOCUS"},
    {0x0008, "CAMERA_MSG_ZOOM"},
    {0x0010, "CAMERA_MSG_PREVIEW_FRAME"},
    {0x0020, "CAMERA_MSG_VIDEO_FRAME"},
    {0x0040, "CAMERA_MSG_POSTVIEW_FRAME"},
    {0x0080, "CAMERA_MSG_RAW_IMAGE"},
    {0x0100, "CAMERA_MSG_COMPRESSED_IMAGE"},
    {0x0200, "CAMERA_MSG_RAW_IMAGE_NOTIFY"},
    {0x0400, "CAMERA_MSG_PREVIEW_METADATA"},
    {0x0000, "CAMERA_MSG_ALL_MSGS"}, //0xFFFF
    {0x0000, "NULL"},
};

android::String8          g_str;
android::CameraParameters camSettings;
preview_stream_ops_t      *mWindow = NULL;
android::sp<android::CameraHardwareInterface> qCamera;

camera_notify_callback         origNotify_cb    = NULL;
camera_data_callback           origData_cb      = NULL;
camera_data_timestamp_callback origDataTS_cb    = NULL;
camera_request_memory          origCamReqMemory = NULL;


static void dump_msg(const char *tag, int msg_type)
{
    int i;
    for (i = 0; msg_map[i].type; i++) {
        if (msg_type & msg_map[i].type) {
            LOGI("%s: %s", tag, msg_map[i].text);
        }
    }
}

void CameraHal_Decode_Sw(unsigned int* rgb, char* yuv420sp, int width, int height)
{
   int frameSize = width * height;
   int yp = 0;
   for (int j = 0, yp = 0; j < height; j++) {
      int uvp = frameSize + (j >> 1) * width, u = 0, v = 0;
      for (int i = 0; i < width; i++, yp++) {
         int y = (0xff & ((int) yuv420sp[yp])) - 16;
         if (y < 0) y = 0;
         if ((i & 1) == 0) {
            v = (0xff & yuv420sp[uvp++]) - 128;
            u = (0xff & yuv420sp[uvp++]) - 128;
         }

         int y1192 = 1192 * y;
         int r = (y1192 + 1634 * v);
         int g = (y1192 - 833 * v - 400 * u);
         int b = (y1192 + 2066 * u);

		 if (r < 0) r = 0; else if (r > 262143) r = 262143;
         if (g < 0) g = 0; else if (g > 262143) g = 262143;
         if (b < 0) b = 0; else if (b > 262143) b = 262143;

         rgb[yp] = 0xff000000 | ((b << 6) & 0xff0000) | ((g >> 2) & 0xff00) | ((r >> 10) & 0xff);
      }
   }
}

void CameraHAL_CopyBuffers_Sw(char *dest, char *src, int size)
{
   int       i;
   int       numWords  = size / sizeof(unsigned);
   unsigned *srcWords  = (unsigned *)src;
   unsigned *destWords = (unsigned *)dest;

   for (i = 0; i < numWords; i++) {
      if ((i % 8) == 0 && (i + 8) < numWords) {
         __builtin_prefetch(srcWords  + 8, 0, 0);
         __builtin_prefetch(destWords + 8, 1, 0);
      }
      *destWords++ = *srcWords++;
   }
   if (__builtin_expect((size - (numWords * sizeof(unsigned))) > 0, 0)) {
      int numBytes = size - (numWords * sizeof(unsigned));
      char *destBytes = (char *)destWords;
      char *srcBytes  = (char *)srcWords;
      for (i = 0; i < numBytes; i++) {
         *destBytes++ = *srcBytes++;
      }
   }
}

camera_memory_t * CameraHAL_GenClientData(const android::sp<android::IMemory> &dataPtr, camera_request_memory reqClientMemory, void *user)
{
   ssize_t          offset;
   size_t           size;
   camera_memory_t *clientData				= NULL;
   android::sp<android::IMemoryHeap> mHeap	= dataPtr->getMemory(&offset, &size);

   LOGV("CameraHAL_GenClientData: offset:%#x size:%#x base:%p\n", (unsigned)offset, size, mHeap != NULL ? mHeap->base() : 0);

	clientData = reqClientMemory(-1, size, 1, user);
	if (clientData != NULL) {
		CameraHAL_CopyBuffers_Sw((char *)clientData->data, (char *)(mHeap->base()) + offset, size);
	} else {
		LOGE("CameraHAL_GenClientData: ERROR allocating memory from client\n");
	}
	return clientData;
}


void CameraHAL_HandlePreviewData(const sp<IMemory>& dataPtr, preview_stream_ops_t *mWindow, camera_request_memory getMemory, int32_t previewWidth, int32_t previewHeight)
{
	if (mWindow != NULL && getMemory != NULL) {
		ssize_t  offset;
		size_t   size;
		int32_t  previewFormat = MDP_Y_CBCR_H2V2;
		int32_t  destFormat    = MDP_RGBA_8888;

		android::status_t retVal;
		sp<IMemoryHeap> mHeap = dataPtr->getMemory(&offset, &size);

		LOGV("CameraHAL_HandlePreviewData: previewWidth:%d previewHeight:%d offset:%#x size:%#x base:%p\n", previewWidth, previewHeight, (unsigned)offset, size, mHeap != NULL ? mHeap->base() : 0);

		mWindow->set_usage(mWindow, GRALLOC_USAGE_PMEM_PRIVATE_ADSP | GRALLOC_USAGE_SW_READ_OFTEN);

		retVal = mWindow->set_buffers_geometry(mWindow, previewWidth, previewHeight, HAL_PIXEL_FORMAT_RGBX_8888);
		if (retVal == NO_ERROR) {
			int32_t          stride;
			buffer_handle_t *bufHandle = NULL;

			LOGV("CameraHAL_HandlePreviewData: dequeueing buffer\n");
			retVal = mWindow->dequeue_buffer(mWindow, &bufHandle, &stride);
			if (retVal == NO_ERROR) {
				retVal = mWindow->lock_buffer(mWindow, bufHandle);
				if (retVal == NO_ERROR) {
					private_handle_t const *privHandle = reinterpret_cast<private_handle_t const *>(*bufHandle);
					void *bits;
					android::Rect bounds;
					android::GraphicBufferMapper &mapper = android::GraphicBufferMapper::get();

					bounds.left   = 0;
					bounds.top    = 0;
					bounds.right  = previewWidth;
					bounds.bottom = previewHeight;

					mapper.lock(*bufHandle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &bits);
					LOGV("CameraHAL_HPD: w:%d h:%d bits:%p\n", previewWidth, previewHeight, bits);
					CameraHal_Decode_Sw((unsigned int *)bits, (char *)mHeap->base() + offset, previewWidth, previewHeight);
					// unlock buffer before sending to display
					mapper.unlock(*bufHandle);

					mWindow->enqueue_buffer(mWindow, bufHandle);
					LOGV("CameraHAL_HandlePreviewData: enqueued buffer\n");
				} else {
					LOGV("CameraHAL_HandlePreviewData: ERROR locking the buffer\n");
					mWindow->cancel_buffer(mWindow, bufHandle);
				}
			} else {
				LOGV("CameraHAL_HandlePreviewData: ERROR dequeueing the buffer\n");
			}
		}
	}
}

static void wrap_notify_callback(int32_t msg_type, int32_t ext1, int32_t ext2, void* user)
{
	LOGV("CameraHAL_NotifyCb: msg_type:%d ext1:%d ext2:%d user:%p\n", msg_type, ext1, ext2, user);
	if (origNotify_cb != NULL) {
		origNotify_cb(msg_type, ext1, ext2, user);
	}

	LOGV("%s---", __FUNCTION__);
}

static void wrap_data_callback(int32_t msg_type, const sp<IMemory>& dataPtr, void* user)
{
	LOGV("wrap_data_callback: msg_type:%d user:%p\n", msg_type, user);

	if (msg_type == CAMERA_MSG_PREVIEW_FRAME) {

		int32_t previewWidth, previewHeight;
		android::CameraParameters hwParameters = qCamera->getParameters();
		hwParameters.getPreviewSize(&previewWidth, &previewHeight);
		CameraHAL_HandlePreviewData(dataPtr, mWindow, origCamReqMemory, previewWidth, previewHeight);

	} else if (origData_cb  != NULL && origCamReqMemory != NULL) {

		camera_memory_t *clientData = CameraHAL_GenClientData(dataPtr, origCamReqMemory, user);
		if (clientData != NULL) {
			LOGV("CameraHAL_DataCb: Posting data to client\n");
			origData_cb(msg_type, clientData, 0, NULL, user);
			clientData->release(clientData);
		}
	} 
	LOGV("wrap_data_callbaak--");
}

static void wrap_data_callback_timestamp(nsecs_t timestamp, int32_t msg_type, const sp<IMemory>& dataPtr, void* user)
{
	if (origDataTS_cb != NULL && origCamReqMemory != NULL) {
		camera_memory_t *clientData = CameraHAL_GenClientData(dataPtr, origCamReqMemory, user);
		if (clientData != NULL) {
			LOGV("CameraHAL_DataTSCb: Posting data to client timestamp:%lld\n", systemTime());
			origDataTS_cb(timestamp, msg_type, clientData, 0, user);
			qCamera->releaseRecordingFrame(dataPtr);
			clientData->release(clientData);
		} else {
			LOGD("CameraHAL_DataTSCb: ERROR allocating memory from client\n");
		}
	}
}

/*******************************************************************
 * implementation of camera_device_ops functions
 *******************************************************************/

void CameraHAL_FixupParams(android::CameraParameters &camParams)
{
    const char *preferred_size = "320x240";
    const char *preview_frame_rates  = "30,27,24,15";
    const char *preferred_rate = "30";

    camParams.set(android::CameraParameters::KEY_VIDEO_FRAME_FORMAT, android::CameraParameters::PIXEL_FORMAT_YUV420SP);
    camParams.set(android::CameraParameters::KEY_PICTURE_FORMAT, android::CameraParameters::PIXEL_FORMAT_JPEG);
    camParams.set(android::CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS, android::CameraParameters::PIXEL_FORMAT_JPEG);
    camParams.set(android::CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, preferred_size);

    camParams.set(android::CameraParameters::KEY_MAX_SHARPNESS, "30");
    camParams.set(android::CameraParameters::KEY_MAX_CONTRAST, "10");
    camParams.set(android::CameraParameters::KEY_MAX_SATURATION, "10");
    camParams.set("num-snaps-per-shutter", "1");
    camParams.set(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES, preview_frame_rates);
 //   camParams.set(CameraParameters::KEY_PREVIEW_FRAME_RATE, preferred_rate);
}

int camera_set_preview_window(struct camera_device * device, struct preview_stream_ops *window) {
	LOGV("qcamera_set_preview_window : Window :%p\n", window);
	if (device == NULL) {
		LOGE("qcamera_set_preview_window : Invalid device.\n");
		return -EINVAL;
	} else {
		LOGV("qcamera_set_preview_window : window :%p\n", window);
		mWindow = window;
		return 0;
	}
}

void camera_set_callbacks(struct camera_device * device,
                          camera_notify_callback notify_cb,
                          camera_data_callback data_cb,
                          camera_data_timestamp_callback data_cb_timestamp,
                          camera_request_memory get_memory,
                          void *user)
{
	origNotify_cb    = notify_cb;
	origData_cb      = data_cb;
	origDataTS_cb    = data_cb_timestamp;
	origCamReqMemory = get_memory;

    qCamera->setCallbacks(wrap_notify_callback, wrap_data_callback, wrap_data_callback_timestamp, user);

    LOGI("%s---", __FUNCTION__);

}

void camera_enable_msg_type(struct camera_device * device, int32_t msg_type)
{
	LOGI("%s+++: type %i", __FUNCTION__, msg_type);
	//if (msg_type & CAMERA_MSG_RAW_IMAGE_NOTIFY) {
	//    msg_type &= ~CAMERA_MSG_RAW_IMAGE_NOTIFY;
	//    msg_type |= CAMERA_MSG_RAW_IMAGE;
	//	}
	if (msg_type == 0xfff) {
		msg_type = 0x1ff;
	} else {
		msg_type &= ~(CAMERA_MSG_PREVIEW_METADATA | CAMERA_MSG_RAW_IMAGE_NOTIFY);
	}
	//   dump_msg(__FUNCTION__, msg_type);

	qCamera->enableMsgType(msg_type);
	LOGI("%s---", __FUNCTION__);

}

void camera_disable_msg_type(struct camera_device * device, int32_t msg_type)
{
    LOGI("%s+++: type %i", __FUNCTION__, msg_type);
    //dump_msg(__FUNCTION__, msg_type);
	if (msg_type == 0xfff) {
		msg_type = 0x1ff;
	}
    qCamera->disableMsgType(msg_type);
    LOGI("%s---", __FUNCTION__);

}

int camera_msg_type_enabled(struct camera_device * device, int32_t msg_type)
{
    LOGI("%s+++: type %i", __FUNCTION__, msg_type);
    return qCamera->msgTypeEnabled(msg_type);
}

int camera_start_preview(struct camera_device * device)
{
	LOGI("%s+++", __FUNCTION__);

	if (!qCamera->msgTypeEnabled(CAMERA_MSG_PREVIEW_FRAME)) {
		qCamera->enableMsgType(CAMERA_MSG_PREVIEW_FRAME);
	}

	return qCamera->startPreview();
}

void camera_stop_preview(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    qCamera->stopPreview();
}

int camera_preview_enabled(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    return qCamera->previewEnabled() ? 1 : 0;
}

int camera_store_meta_data_in_buffers(struct camera_device * device, int enable)
{
    return NO_ERROR;
}

int camera_start_recording(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
	qCamera->enableMsgType(CAMERA_MSG_VIDEO_FRAME);
    return qCamera->startRecording();
}

void camera_stop_recording(struct camera_device * device)
{
    LOGI("%s+++: device", __FUNCTION__);

	qCamera->disableMsgType(CAMERA_MSG_VIDEO_FRAME);
    qCamera->stopRecording();

    //qCamera->startPreview();
    LOGI("%s---", __FUNCTION__);
}

int camera_recording_enabled(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    return qCamera->recordingEnabled() ? 1 : 0;
}

void camera_release_recording_frame(struct camera_device * device, const void *opaque)
{
    LOGI("%s---", __FUNCTION__);
}

int camera_auto_focus(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    return qCamera->autoFocus();
}

int camera_cancel_auto_focus(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    return qCamera->cancelAutoFocus();
}

int camera_take_picture(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);

    qCamera->enableMsgType(CAMERA_MSG_SHUTTER | CAMERA_MSG_POSTVIEW_FRAME | CAMERA_MSG_RAW_IMAGE | CAMERA_MSG_COMPRESSED_IMAGE);
    return qCamera->takePicture();
}

int camera_cancel_picture(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    return qCamera->cancelPicture();
}

int camera_set_parameters(struct camera_device * device, const char *params)
{
   LOGV("qcamera_set_parameters: %s\n", params);
   g_str = android::String8(params);
   camSettings.unflatten(g_str);
   qCamera->setParameters(camSettings);
   return NO_ERROR;
}

char* camera_get_parameters(struct camera_device * device)
{
   char *rc = NULL;
   LOGV("qcamera_get_parameters\n");
   camSettings = qCamera->getParameters();
   LOGV("qcamera_get_parameters: after calling qCamera->getParameters()\n");
   CameraHAL_FixupParams(camSettings);
   g_str = camSettings.flatten();
   rc = strdup((char *)g_str.string());
   LOGV("camera_get_parameters: returning rc:%p :%s\n", rc, (rc != NULL) ? rc : "EMPTY STRING");
   return rc;
}

static void camera_put_parameters(struct camera_device *device, char *parms)
{
    LOGI("%s+++", __FUNCTION__);
    free(parms);
    LOGI("%s---", __FUNCTION__);
}

int camera_send_command(struct camera_device * device, int32_t cmd, int32_t arg1, int32_t arg2)
{
    LOGI("%s: cmd %i", __FUNCTION__, cmd);
    return qCamera->sendCommand(cmd, arg1, arg2);
}

void camera_release(struct camera_device * device)
{
    LOGI("%s+++", __FUNCTION__);
    qCamera->release();
    LOGI("%s---", __FUNCTION__);
}

int camera_dump(struct camera_device * device, int fd)
{
    LOGI("%s", __FUNCTION__);
   // return qCamera->dump(device, fd);
    return NO_ERROR;
}

extern "C" void heaptracker_free_leaked_memory(void);

int camera_device_close(hw_device_t* device)
{
	int rc = -EINVAL;
	LOGD("camera_device_close\n");
	camera_device_t *cameraDev = (camera_device_t *)device;
	if (cameraDev) {
		//delete qCamera;
		qCamera = NULL;
		if (cameraDev->ops) {
			free(cameraDev->ops);
		}
		free(cameraDev);
		rc = NO_ERROR;
	}
	return rc;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

int camera_device_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    int cameraid;
    int num_cameras				= 0;
    camera_device_t* 		camera_device	= NULL;
    camera_device_ops_t* camera_ops		= NULL;
    int rv					= 0;

    LOGI("camera_device open+++");

    if (name != NULL) {
        cameraid = atoi(name);

        num_cameras = HAL_getNumberOfCameras();

        if(cameraid > num_cameras)
        {
            LOGE("camera service provided cameraid out of bounds, "
                 "cameraid = %d, num supported = %d",
                 cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

	qCamera = HAL_openCameraHardware(cameraid);

        camera_device = (camera_device_t*)malloc(sizeof(*camera_device));
        if(!camera_device)
        {
            LOGE("camera_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
        if(!camera_ops)
        {
            LOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(camera_device, 0, sizeof(*camera_device));
        memset(camera_ops, 0, sizeof(*camera_ops));

        camera_device->common.tag			= HARDWARE_DEVICE_TAG;
        camera_device->common.version			= 0;
        camera_device->common.module			= (hw_module_t *)(module);
        camera_device->common.close			= camera_device_close;
        camera_device->ops				= camera_ops;

        camera_ops->set_preview_window			= camera_set_preview_window;
        camera_ops->set_callbacks			= camera_set_callbacks;
        camera_ops->enable_msg_type			= camera_enable_msg_type;
        camera_ops->disable_msg_type			= camera_disable_msg_type;
        camera_ops->msg_type_enabled			= camera_msg_type_enabled;
        camera_ops->start_preview			= camera_start_preview;
        camera_ops->stop_preview			= camera_stop_preview;
        camera_ops->preview_enabled			= camera_preview_enabled;
        camera_ops->store_meta_data_in_buffers		= camera_store_meta_data_in_buffers;
        camera_ops->start_recording			= camera_start_recording;
        camera_ops->stop_recording			= camera_stop_recording;
        camera_ops->recording_enabled			= camera_recording_enabled;
        camera_ops->release_recording_frame		= camera_release_recording_frame;
        camera_ops->auto_focus				= camera_auto_focus;
        camera_ops->cancel_auto_focus			= camera_cancel_auto_focus;
        camera_ops->take_picture			= camera_take_picture;
        camera_ops->cancel_picture			= camera_cancel_picture;
        camera_ops->set_parameters			= camera_set_parameters;
        camera_ops->get_parameters			= camera_get_parameters;
        camera_ops->put_parameters			= camera_put_parameters;
        camera_ops->send_command			= camera_send_command;
        camera_ops->release				= camera_release;
        camera_ops->dump				= camera_dump;

        *device = &camera_device->common;

        // -------- specific stuff --------
        
        if(qCamera == NULL)
        {
            LOGE("Couldn't create instance of CameraHal class");
            rv = -ENOMEM;
            goto fail;
        }
    }
    LOGI("%s---ok rv %d", __FUNCTION__,rv);

    return rv;

fail:
    if(camera_device) {
        free(camera_device);
        camera_device = NULL;
    }
    if(camera_ops) {
        free(camera_ops);
        camera_ops = NULL;
    }
    *device = NULL;
    LOGI("%s--- fail rv %d", __FUNCTION__,rv);

    return rv;
}

int camera_get_number_of_cameras(void)
{
    int num_cameras = HAL_getNumberOfCameras();

    LOGI("%s: number:%i", __FUNCTION__, num_cameras);

    return num_cameras;
}

int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    int rv = 0;

    CameraInfo cameraInfo;

    android::HAL_getCameraInfo(camera_id, &cameraInfo);

    info->facing = cameraInfo.facing;
    //info->orientation = cameraInfo.orientation;
    if(info->facing == 1) {
        info->orientation = 270;
    } else {
        info->orientation = 90;
    }

    LOGI("%s: id:%i faceing:%i orientation: %i", __FUNCTION__,camera_id, info->facing, info->orientation);
    return rv;
}
