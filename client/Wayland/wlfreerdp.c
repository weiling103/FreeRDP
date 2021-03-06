/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Wayland Client
 *
 * Copyright 2014 Manuel Bachmann <tarnyko@tarnyko.net>
 * Copyright 2016 Thincast Technologies GmbH
 * Copyright 2016 Armin Novak <armin.novak@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <errno.h>
#include <locale.h>

#include <freerdp/client/cmdline.h>
#include <freerdp/channels/channels.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client.h>
#include <freerdp/utils/signal.h>
#include <freerdp/locale/keyboard.h>

#include <linux/input.h>

#include <uwac/uwac.h>

#include "wlfreerdp.h"
#include "wlf_input.h"
#include "wlf_channels.h"

static BOOL wl_update_content(wlfContext* context_w)
{
	if (!context_w)
		return FALSE;

	if (!context_w->waitingFrameDone && context_w->haveDamage)
	{
		UwacWindowSubmitBuffer(context_w->window, true);
		context_w->waitingFrameDone = TRUE;
		context_w->haveDamage = FALSE;
	}

	return TRUE;
}

static BOOL wl_begin_paint(rdpContext* context)
{
	rdpGdi* gdi;

	if (!context || !context->gdi)
		return FALSE;

	gdi = context->gdi;

	if (!gdi->primary)
		return FALSE;

	gdi->primary->hdc->hwnd->invalid->null = TRUE;
	return TRUE;
}

static BOOL wl_update_buffer(wlfContext* context_w, UINT32 x, UINT32 y, UINT32 w, UINT32 h)
{
	rdpGdi* gdi;
	char* data;
	UINT32 i;

	if (!context_w)
		return FALSE;

	data = UwacWindowGetDrawingBuffer(context_w->window);

	if (!data)
		return FALSE;

	gdi = context_w->context.gdi;

	if (!gdi)
		return FALSE;

	for (i = 0; i < h; i++)
	{
		memcpy(data + ((i + y) * (gdi->width * GetBytesPerPixel(
		                              gdi->dstFormat))) + x * GetBytesPerPixel(gdi->dstFormat),
		       gdi->primary_buffer + ((i + y) * (gdi->width * GetBytesPerPixel(
		                                  gdi->dstFormat))) + x * GetBytesPerPixel(gdi->dstFormat),
		       w * GetBytesPerPixel(gdi->dstFormat));
	}

	if (UwacWindowAddDamage(context_w->window, x, y, w, h) != UWAC_SUCCESS)
		return FALSE;

	context_w->haveDamage = TRUE;
	return wl_update_content(context_w);
}

static BOOL wl_end_paint(rdpContext* context)
{
	rdpGdi* gdi;
	wlfContext* context_w;
	INT32 x, y;
	UINT32 w, h;

	if (!context || !context->gdi || !context->gdi->primary)
		return FALSE;

	gdi = context->gdi;

	if (gdi->primary->hdc->hwnd->invalid->null)
		return TRUE;

	x = gdi->primary->hdc->hwnd->invalid->x;
	y = gdi->primary->hdc->hwnd->invalid->y;
	w = gdi->primary->hdc->hwnd->invalid->w;
	h = gdi->primary->hdc->hwnd->invalid->h;
	context_w = (wlfContext*) context;
	return wl_update_buffer(context_w, x, y, w, h);
}

static BOOL wl_refresh_display(wlfContext* context)
{
	rdpGdi* gdi;

	if (!context || !context->context.gdi)
		return FALSE;

	gdi = context->context.gdi;
	return wl_update_buffer(context, 0, 0, gdi->width, gdi->height);
}

static BOOL wl_resize_display(rdpContext* context)
{
	wlfContext* wlc = (wlfContext*)context;
	rdpGdi* gdi = context->gdi;
	rdpSettings* settings = context->settings;

	if (!gdi_resize(gdi, settings->DesktopWidth, settings->DesktopHeight))
		return FALSE;

	return wl_refresh_display(wlc);
}

static BOOL wl_pre_connect(freerdp* instance)
{
	rdpSettings* settings;
	wlfContext* context;
	UwacOutput* output;
	UwacSize resolution;

	if (!instance)
		return FALSE;

	context = (wlfContext*) instance->context;
	settings = instance->settings;

	if (!context || !settings)
		return FALSE;

	settings->OsMajorType = OSMAJORTYPE_UNIX;
	settings->OsMinorType = OSMINORTYPE_NATIVE_WAYLAND;
	PubSub_SubscribeChannelConnected(instance->context->pubSub,
	                                 wlf_OnChannelConnectedEventHandler);
	PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                    wlf_OnChannelDisconnectedEventHandler);

	if (settings->Fullscreen)
	{
		// Use the resolution of the first display output
		output = UwacDisplayGetOutput(context->display, 1);

		if (output != NULL && UwacOutputGetResolution(output, &resolution) == UWAC_SUCCESS)
		{
			settings->DesktopWidth = (UINT32) resolution.width;
			settings->DesktopHeight = (UINT32) resolution.height;
		}
		else
		{
			WLog_WARN(TAG, "Failed to get output resolution! Check your display settings");
		}
	}

	if (!freerdp_client_load_addins(instance->context->channels,
	                                instance->settings))
		return FALSE;

	return TRUE;
}

static BOOL wl_post_connect(freerdp* instance)
{
	rdpGdi* gdi;
	UwacWindow* window;
	wlfContext* context;

	if (!instance || !instance->context)
		return FALSE;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
		return FALSE;

	gdi = instance->context->gdi;

	if (!gdi)
		return FALSE;

	context = (wlfContext*) instance->context;
	context->window = window = UwacCreateWindowShm(context->display, gdi->width,
	                           gdi->height, WL_SHM_FORMAT_XRGB8888);

	if (!window)
		return FALSE;

	UwacWindowSetFullscreenState(window, NULL, instance->context->settings->Fullscreen);
	UwacWindowSetTitle(window, "FreeRDP");
	UwacWindowSetOpaqueRegion(context->window, 0, 0, gdi->width, gdi->height);
	instance->update->BeginPaint = wl_begin_paint;
	instance->update->EndPaint = wl_end_paint;
	instance->update->DesktopResize = wl_resize_display;
	freerdp_keyboard_init(instance->context->settings->KeyboardLayout);
	return wl_update_buffer(context, 0, 0, gdi->width, gdi->height);
}

static void wl_post_disconnect(freerdp* instance)
{
	wlfContext* context;

	if (!instance)
		return;

	if (!instance->context)
		return;

	context = (wlfContext*) instance->context;
	gdi_free(instance);

	if (context->window)
		UwacDestroyWindow(&context->window);
}

static BOOL handle_uwac_events(freerdp* instance, UwacDisplay* display)
{
	UwacEvent event;
	wlfContext* context;

	if (UwacDisplayDispatch(display, 1) < 0)
		return FALSE;

	context = (wlfContext*)instance->context;

	while (UwacHasEvent(display))
	{
		if (UwacNextEvent(display, &event) != UWAC_SUCCESS)
			return FALSE;

		/*printf("UWAC event type %d\n", event.type);*/
		switch (event.type)
		{
			case UWAC_EVENT_FRAME_DONE:
				if (!instance)
					continue;

				context->waitingFrameDone = FALSE;

				if (context->haveDamage && !wl_update_content(context))
					return FALSE;

				break;

			case UWAC_EVENT_POINTER_ENTER:
				if (!wlf_handle_pointer_enter(instance, &event.mouse_enter_leave))
					return FALSE;

				break;

			case UWAC_EVENT_POINTER_MOTION:
				if (!wlf_handle_pointer_motion(instance, &event.mouse_motion))
					return FALSE;

				break;

			case UWAC_EVENT_POINTER_BUTTONS:
				if (!wlf_handle_pointer_buttons(instance, &event.mouse_button))
					return FALSE;

				break;

			case UWAC_EVENT_POINTER_AXIS:
				if (!wlf_handle_pointer_axis(instance, &event.mouse_axis))
					return FALSE;

				break;

			case UWAC_EVENT_KEY:
				if (!wlf_handle_key(instance, &event.key))
					return FALSE;

				break;

			case UWAC_EVENT_KEYBOARD_ENTER:
				if (instance->context->settings->GrabKeyboard)
					UwacSeatInhibitShortcuts(event.keyboard_enter_leave.seat, true);
				if (!wlf_keyboard_enter(instance, &event.keyboard_enter_leave))
					return FALSE;

				break;

			case UWAC_EVENT_CONFIGURE:
				if (!wl_refresh_display(context))
					return FALSE;

				break;

			default:
				break;
		}
	}

	return TRUE;
}

static int wlfreerdp_run(freerdp* instance)
{
	wlfContext* context;
	DWORD count;
	HANDLE handles[64];
	DWORD status = WAIT_ABANDONED;

	if (!instance)
		return -1;

	context = (wlfContext*)instance->context;

	if (!context)
		return -1;

	if (!freerdp_connect(instance))
	{
		printf("Failed to connect\n");
		return -1;
	}

	while (!freerdp_shall_disconnect(instance))
	{
		handles[0] = context->displayHandle;
		count = freerdp_get_event_handles(instance->context, &handles[1], 63) + 1;

		if (count <= 1)
		{
			printf("Failed to get FreeRDP file descriptor\n");
			break;
		}

		status = WaitForMultipleObjects(count, handles, FALSE, INFINITE);

		if (WAIT_FAILED == status)
		{
			printf("%s: WaitForMultipleObjects failed\n", __FUNCTION__);
			break;
		}

		if (!handle_uwac_events(instance, context->display))
		{
			printf("error handling UWAC events\n");
			break;
		}

		if (freerdp_check_event_handles(instance->context) != TRUE)
		{
			if (freerdp_get_last_error(instance->context) == FREERDP_ERROR_SUCCESS)
				printf("Failed to check FreeRDP file descriptor\n");

			break;
		}
	}

	freerdp_disconnect(instance);
	return status;
}

static BOOL wlf_client_global_init(void)
{
	setlocale(LC_ALL, "");

	if (freerdp_handle_signals() != 0)
		return FALSE;

	return TRUE;
}

static void wlf_client_global_uninit(void)
{
}

static int wlf_logon_error_info(freerdp* instance, UINT32 data, UINT32 type)
{
	wlfContext* wlf;
	const char* str_data = freerdp_get_logon_error_info_data(data);
	const char* str_type = freerdp_get_logon_error_info_type(type);

	if (!instance || !instance->context)
		return -1;

	wlf = (wlfContext*) instance->context;
	WLog_INFO(TAG, "Logon Error Info %s [%s]", str_data, str_type);
	return 1;
}

static BOOL wlf_client_new(freerdp* instance, rdpContext* context)
{
	UwacReturnCode status;
	wlfContext* wfl = (wlfContext*) context;

	if (!instance || !context)
		return FALSE;

	instance->PreConnect = wl_pre_connect;
	instance->PostConnect = wl_post_connect;
	instance->PostDisconnect = wl_post_disconnect;
	instance->Authenticate = client_cli_authenticate;
	instance->GatewayAuthenticate = client_cli_gw_authenticate;
	instance->VerifyCertificateEx = client_cli_verify_certificate_ex;
	instance->VerifyChangedCertificateEx = client_cli_verify_changed_certificate_ex;
	instance->LogonErrorInfo = wlf_logon_error_info;
	wfl->display = UwacOpenDisplay(NULL, &status);

	if (!wfl->display || (status != UWAC_SUCCESS))
		return FALSE;

	wfl->displayHandle = CreateFileDescriptorEvent(NULL, FALSE, FALSE,
	                     UwacDisplayGetFd(wfl->display), WINPR_FD_READ);

	if (!wfl->displayHandle)
		return FALSE;

	return TRUE;
}


static void wlf_client_free(freerdp* instance, rdpContext* context)
{
	wlfContext* wlf = (wlfContext*) instance->context;

	if (!context)
		return;

	if (wlf->display)
		UwacCloseDisplay(&wlf->display);

	if (wlf->displayHandle)
		CloseHandle(wlf->displayHandle);
}

static int wfl_client_start(rdpContext* context)
{
	return 0;
}

static int wfl_client_stop(rdpContext* context)
{
	return 0;
}

static int RdpClientEntry(RDP_CLIENT_ENTRY_POINTS* pEntryPoints)
{
	ZeroMemory(pEntryPoints, sizeof(RDP_CLIENT_ENTRY_POINTS));
	pEntryPoints->Version = RDP_CLIENT_INTERFACE_VERSION;
	pEntryPoints->Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	pEntryPoints->GlobalInit = wlf_client_global_init;
	pEntryPoints->GlobalUninit = wlf_client_global_uninit;
	pEntryPoints->ContextSize = sizeof(wlfContext);
	pEntryPoints->ClientNew = wlf_client_new;
	pEntryPoints->ClientFree = wlf_client_free;
	pEntryPoints->ClientStart = wfl_client_start;
	pEntryPoints->ClientStop = wfl_client_stop;
	return 0;
}

int main(int argc, char* argv[])
{
	int rc = -1;
	DWORD status;
	RDP_CLIENT_ENTRY_POINTS clientEntryPoints;
	rdpContext* context;
	RdpClientEntry(&clientEntryPoints);
	context = freerdp_client_context_new(&clientEntryPoints);

	if (!context)
		goto fail;

	status = freerdp_client_settings_parse_command_line(context->settings, argc,
	         argv, FALSE);
	status = freerdp_client_settings_command_line_status_print(context->settings,
	         status, argc, argv);

	if (status)
		return 0;

	if (freerdp_client_start(context) != 0)
		goto fail;

	rc = wlfreerdp_run(context->instance);

	if (freerdp_client_stop(context) != 0)
		rc = -1;

fail:
	freerdp_client_context_free(context);
	return rc;
}
