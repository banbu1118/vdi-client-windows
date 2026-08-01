#include "freerdp/codec/region.h"
#include "freerdp/event.h"
#include "freerdp/gdi/gdi.h"
#include "freerdp/settings_keys.h"
#include <cwchar>
#include <freerdp/addin.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/audin.h>
#include <freerdp/channels/urbdrc.h>
#include <freerdp/channels/rdpecam.h>
#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/disp.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/graphics.h>
#include <freerdp/settings.h>
#include <freerdp/types.h>
#include <freerdp/update.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmeapi.h>  // waveInGetNumDevs
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <QIcon>
#include <QGuiApplication>
#include <QCursor>
#include <QImage>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QClipboard>
#include <QBuffer>
#include <QMimeData>
#include <QTimer>
#include <map>

#include "qf_util.h"
#include "qf_log.h"
#include "clipboard-entry.h"
#include "rdp-view-item.h"
#include "usb-manager.h"

extern "C" BOOL VCAPITYPE cliprdr_VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS_EX pEntryPoints,
                                                        PVOID pInitHandle);


/* Saved original FreeRDP addin provider for chaining */
static FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN g_orig_addin_provider = nullptr;

/* Custom addin provider that chains to the original provider. */
static PVIRTUALCHANNELENTRY qf_addin_provider(LPCSTR pszName, LPCSTR pszSubsystem,
                                              LPCSTR pszType, DWORD dwFlags)
{
	/* Fall through to original provider */
	if (g_orig_addin_provider)
		return g_orig_addin_provider(pszName, pszSubsystem, pszType, dwFlags);
	return nullptr;
}

static std::atomic<bool> g_stopped = false;
static RdpViewItem* g_rdpViewItem = nullptr;
static std::unique_ptr<std::thread> g_freerdp_thread = nullptr;
static int g_cli_argc = 0;
static char** g_cli_argv = nullptr;
static bool g_cli_parsed = false;
/* Resolution passed via command-line /w: and /h: (0 = not specified) */
static uint32_t g_cli_width = 0;
static uint32_t g_cli_height = 0;
static bool g_usb_cli_enabled = false; // 由 CLI（/usb:）或 .rdp 文件（usbdevicestoredirect）启用
static std::string g_saved_usb_value;  // 保存的 USB 值，用于重连时恢复
static std::vector<std::string> g_saved_drive_args;  // 保存的 /drive: 参数，用于重连时恢复
static std::shared_ptr<qf::client_t> g_client = {};
static std::unique_ptr<qf::clipboard_entry> g_clipboard_entry = nullptr;
static std::unique_ptr<USBManager> g_usbManager;
static std::atomic<bool> g_reconnectRequested{false};
static freerdp* g_instance = nullptr;
static CliprdrClientContext* g_clipboard_client_context = nullptr;
static DispClientContext* g_dispContext = nullptr;
static RdpgfxClientContext* g_gfxContext = nullptr;

/* =====================================================================
 * Resize debug tracking — helps diagnose why dynamic resolution
 * sometimes fails to take effect.
 *
 * g_last_disp_send_ts:  steady clock when SendMonitorLayout was last called
 * g_pending_resize_count: number of resize requests sent but not yet
 *                         acknowledged by a GFX_RESET (noop_desktop_resize)
 * g_last_disp_w/h:       last aligned size sent to the server
 * g_last_gfx_resize_w/h: size received in the GFX reset response
 * ===================================================================== */
static std::chrono::steady_clock::time_point g_last_disp_send_ts{};
static std::atomic<int> g_pending_resize_count{0};
static std::atomic<uint32_t> g_last_disp_w{0};
static std::atomic<uint32_t> g_last_disp_h{0};
static std::atomic<uint32_t> g_last_gfx_resize_w{0};
static std::atomic<uint32_t> g_last_gfx_resize_h{0};

/* =====================================================================
 * RDP Pointer (Cursor) Support
 *
 * Renders remote cursor shapes received via RDP Pointer Update PDUs.
 * The server sends SYSPTR_NULL to hide the cursor, SYSPTR_DEFAULT for the
 * arrow, and XOR/AND-mask bitmaps for custom pointers.
 *
 * NOTE: cursor bitmap decoding uses freerdp_image_copy_from_pointer_data()
 * (the official FreeRDP API) instead of manual XOR/AND parsing, because
 * the bitmap encoding (bottom-up, palette, RLE) is non-trivial.
 * ===================================================================== */
static std::unordered_map<rdpPointer*, QCursor> g_pointer_cache;
static std::mutex g_pointer_mutex;
static QCursor g_blank_cursor;
static std::atomic<bool> g_cursor_hidden{false}; /* SYSPTR_NULL active */
static QCursor g_last_rdp_cursor;                 /* last non-null cursor */
static std::atomic<uint64_t> g_last_mouse_move_ms{0}; /* steady_clock ms */

/* Position tracking for hover restore threshold.
 * When the server hides the cursor (setnull) we record the mouse position;
 * rdp_notify_mouse_moved will only restore the cursor once the pointer
 * has moved at least CURSOR_RESTORE_THRESHOLD pixels away, preventing
 * tiny sensor noise from defeating auto-hide. */
static constexpr double CURSOR_RESTORE_THRESHOLD = 3.0;
static double g_hide_pos_x = -9999;
static double g_hide_pos_y = -9999;

static void ensure_blank_cursor()
{
	static bool done = false;
	if (!done)
	{
		QPixmap blank(1, 1);
		blank.fill(Qt::transparent);
		g_blank_cursor = QCursor(blank);
		done = true;
	}
}

/**
 * Called from RdpViewItem::mouseMoveEvent / hoverMoveEvent (GUI thread)
 * on every mouse move.  If the cursor was hidden by the server (SYSPTR_NULL),
 * restore it and record the timestamp so my_pointer_setnull won't
 * immediately re-hide it.
 *
 * When the cursor is NOT hidden the function returns immediately without
 * touching g_last_mouse_move_ms — this is critical so that the 2-second
 * skip window in my_pointer_setnull can eventually expire and allow the
 * server to hide the cursor again.
 */
void rdp_notify_mouse_moved(double qx, double qy)
{
	if (!g_cursor_hidden.load(std::memory_order_acquire))
		return;

	/* Don't restore for tiny movements — require a minimum distance
	 * from the position where setnull hid the cursor.  This prevents
	 * mouse-sensor noise from defeating auto-hide. */
	double dx = qx - g_hide_pos_x;
	double dy = qy - g_hide_pos_y;
	if ((dx * dx + dy * dy) < CURSOR_RESTORE_THRESHOLD * CURSOR_RESTORE_THRESHOLD)
		return;

	/* Cursor was hidden AND moved far enough — restore it */
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	              now.time_since_epoch())
	              .count();
	g_last_mouse_move_ms.store(ms, std::memory_order_release);

	g_cursor_hidden.store(false, std::memory_order_release);
	g_rdpViewItem->setCursor(g_last_rdp_cursor);
}

static BOOL my_pointer_new(rdpContext* context, rdpPointer* pointer)
{
	rdpGdi* gdi = context->gdi;

	const uint32_t w = pointer->width;
	const uint32_t h = pointer->height;

	if (w == 0 || h == 0 || w > 512 || h > 512)
	{
		return TRUE;
	}

	/* Allocate temporary buffer for decoded pixel data (BGRA32 = 4 Bpp) */
	const uint32_t stride = w * 4;
	const size_t bufSize = static_cast<size_t>(stride) * h;
	BYTE* data = static_cast<BYTE*>(winpr_aligned_malloc(bufSize ? bufSize : 1, 16));
	if (!data)
	{
		qf::log::error("rdp/pointer", "alloc {} bytes failed", bufSize);
		return FALSE;
	}

	/* Use FreeRDP's official pointer decoder — handles all XOR/AND formats */
	if (!freerdp_image_copy_from_pointer_data(
	        data, PIXEL_FORMAT_BGRA32, stride, 0, 0, w, h,
	        pointer->xorMaskData, pointer->lengthXorMask,
	        pointer->andMaskData, pointer->lengthAndMask,
	        pointer->xorBpp, &gdi->palette))
	{
		qf::log::warn("rdp/pointer", "freerdp_image_copy_from_pointer_data failed");
		winpr_aligned_free(data);
		return TRUE; /* skip, not fatal */
	}

	/* Wrap decoded data in QImage → QPixmap → QCursor (QPixmap copies) */
	QImage image(data, static_cast<int>(w), static_cast<int>(h),
	             static_cast<int>(stride), QImage::Format_ARGB32);
	QCursor cursor(QPixmap::fromImage(image),
	               static_cast<int>(pointer->xPos),
	               static_cast<int>(pointer->yPos));

	{
		std::lock_guard<std::mutex> lock(g_pointer_mutex);
		g_pointer_cache[pointer] = cursor;
	}

	winpr_aligned_free(data);

	return TRUE;
}

static void my_pointer_free(rdpContext* context, rdpPointer* pointer)
{
	WINPR_UNUSED(context);
	std::lock_guard<std::mutex> lock(g_pointer_mutex);
	g_pointer_cache.erase(pointer);
}

static BOOL my_pointer_set(rdpContext* context, rdpPointer* pointer)
{
	WINPR_UNUSED(context);
	QMetaObject::invokeMethod(g_rdpViewItem,
		[ptr = pointer]()
		{
			std::lock_guard<std::mutex> lock(g_pointer_mutex);
			auto it = g_pointer_cache.find(ptr);
			if (it != g_pointer_cache.end())
			{
				g_last_rdp_cursor = it->second;
				g_cursor_hidden.store(false, std::memory_order_release);
				g_rdpViewItem->setCursor(it->second);
			}
			else
			{
				qf::log::warn("rdp/pointer", "set: cursor {} not in cache", fmt::ptr(ptr));
			}
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setnull(rdpContext* context)
{
	WINPR_UNUSED(context);

	/* If the user moved the mouse within the last 2 seconds, don't hide the
	 * cursor.  This prevents the race where:
	 *   mouse move → restore cursor → send PTR_FLAGS_MOVE → server replies
	 *   SYSPTR_NULL → cursor hidden again before user sees it.
	 * The 2 s window ensures the cursor stays visible while the user is
	 * actively using the mouse, even if the server aggressively hides it
	 * (e.g. fullscreen video). */
	{
		auto now = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		              now.time_since_epoch())
		              .count();
		auto last = g_last_mouse_move_ms.load(std::memory_order_acquire);
		if (last != 0 && ms - last < 2000)
		{
			return TRUE;
		}
	}

	g_cursor_hidden.store(true, std::memory_order_release);
	/* ensure_blank_cursor() already ran in start_rdp_connection() on main thread */
	QMetaObject::invokeMethod(g_rdpViewItem,
		[]()
		{
			/* Record the current mouse position so hoverMoveEvent
			 * can require a minimum travel distance before restoring. */
			QPointF local = g_rdpViewItem->mapFromGlobal(QCursor::pos());
			g_hide_pos_x = local.x();
			g_hide_pos_y = local.y();

			g_rdpViewItem->setCursor(g_blank_cursor);
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setdefault(rdpContext* context)
{
	WINPR_UNUSED(context);
	g_cursor_hidden.store(false, std::memory_order_release);
	QMetaObject::invokeMethod(g_rdpViewItem,
		[]()
		{
			g_rdpViewItem->unsetCursor();
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setposition(rdpContext* context, UINT32 x, UINT32 y)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(x);
	WINPR_UNUSED(y);
	/* Qt manages cursor position natively — nothing to do. */
	return TRUE;
}

static RECTANGLE_16 scale_frame(rdpContext* context, const RECTANGLE_16* rect)
{
	uint32_t hw = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
	uint32_t hh = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
	uint32_t cw = g_client->view_width_;
	uint32_t ch = g_client->view_height_;
	RECTANGLE_16 rect16 = *rect; // copy rect to rect16

	if (cw == 0)
		cw = hw;
	if (ch == 0)
		ch = hh;

	if (cw != hw || hh != ch)
	{
		rect16.left = rect->left * cw / hw;
		rect16.right = rect->right * cw / hw;
		rect16.top = rect->top * ch / hh;
		rect16.bottom = rect->bottom * ch / hh;
	}

	return rect16;
}

static BOOL noop_begin_paint(rdpContext* context)
{
	WINPR_UNUSED(context);
	return TRUE;
}

static BOOL noop_end_paint(rdpContext* context)
{
	rdpGdi* gdi = context->gdi;

	HGDI_DC hdc = gdi->primary->hdc;
	HGDI_WND hwnd = hdc->hwnd;
	REGION16 region16;

	if (!hwnd || hwnd->invalid->null == TRUE)
		return TRUE;

	region16_init(&region16);

	HGDI_RGN cinvalid = hwnd->cinvalid;
	for (int i = 0; i < hwnd->ninvalid; ++i)
	{
		RECTANGLE_16 rect;
		rect.left = cinvalid[i].x;
		rect.top = cinvalid[i].y;
		rect.right = cinvalid[i].x + cinvalid[i].w;
		rect.bottom = cinvalid[i].y + hwnd->cinvalid[i].h;
		region16_union_rect(&region16, &region16, &rect);
	}

	const RECTANGLE_16* rect = nullptr;
	if (!region16_is_empty(&region16))
	{
		rect = region16_extents(static_cast<const REGION16*>(&region16));
	}

	if (rect)
	{
		RECTANGLE_16 update_rect = scale_frame(context, rect);

	int rx = update_rect.left;
		int ry = update_rect.top;
		int rw = update_rect.right - update_rect.left;
		int rh = update_rect.bottom - update_rect.top;

		/*
		 * Copy frame data from GDI buffer to staging buffer on the
		 * FreeRDP thread, while the data is guaranteed to be stable
		 * (EndPaint just completed).  Then queue a GUI thread update.
		 */
		g_rdpViewItem->copyFrameData(gdi->primary_buffer, gdi->stride,
		                             rx, ry, rw, rh);
		QMetaObject::invokeMethod(
		    g_rdpViewItem,
		    [gdi, rx, ry, rw, rh]() {
			    g_rdpViewItem->updateGdiFrame(gdi, rx, ry, rw, rh);
		    });
	}
	region16_uninit(&region16);

	return TRUE;
}

static BOOL noop_desktop_resize(rdpContext* context)
{
	rdpGdi* gdi = context->gdi;
	rdpSettings* settings = context->settings;
	uint32_t newW = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	uint32_t newH = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

	/* DEBUG: track GFX reset vs our last sent size */
	int pending = g_pending_resize_count.load(std::memory_order_acquire);
	uint32_t lastSentW = g_last_disp_w.load(std::memory_order_acquire);
	uint32_t lastSentH = g_last_disp_h.load(std::memory_order_acquire);
	if (pending > 0)
	{
		g_pending_resize_count.fetch_sub(1, std::memory_order_acq_rel);
	}
	else
	{
		qf::log::warn("rdp/resize",
		    "GFX_RESET with pending=0 — server-initiated resize or stale response? "
		    "server={}x{} lastSent={}x{}",
		    newW, newH, lastSentW, lastSentH);
	}

	if (lastSentW != 0 && lastSentH != 0 && (newW != lastSentW || newH != lastSentH))
	{
		qf::log::warn("rdp/resize",
		    "MISMATCH: server returned {}x{} but we last sent {}x{}",
		    newW, newH, lastSentW, lastSentH);
	}
	g_last_gfx_resize_w.store(newW, std::memory_order_release);
	g_last_gfx_resize_h.store(newH, std::memory_order_release);

	qf::log::info("rdp/desktop-resize", "resize to {}x{} (gfx={})", newW, newH,
	              gdi->gfx ? "active" : "none");

	if (!gdi->gfx)
	{
		if (!gdi_resize(gdi, newW, newH))
		{
			return FALSE;
		}
	}
	else
	{
		qf::log::info("rdp/desktop-resize",
		              "GFX active, skipping gdi_resize (already done by gdi_ResetGraphics)");
	}

	/*
	 * Update view dimensions so that scale_frame() in noop_end_paint sees
	 * cw==hw and ch==hh and does NOT apply scaling.
	 */
	g_client->view_width_ = newW;
	g_client->view_height_ = newH;

	/* Resize staging buffer synchronously on RDP thread */
	g_rdpViewItem->resizeStagingBuffer(newW, newH);
	/* Dispatch GUI-thread-only work via invokeMethod. */
	QMetaObject::invokeMethod(g_rdpViewItem, [=]() {
		g_rdpViewItem->notifyFrameResized();
	}, Qt::QueuedConnection);

	qf::log::info("rdp/desktop-resize", "resize complete");

	/* Update RdpViewItem's exposed RDP geometry for QML poll logging */
	if (g_rdpViewItem)
	{
		g_rdpViewItem->setRdpGeometry(static_cast<int>(newW), static_cast<int>(newH));

		double cmpDpr = 1.0;
		if (auto* cmpWin = g_rdpViewItem->window())
			cmpDpr = cmpWin->devicePixelRatio();
		int winW = static_cast<int>(std::round(g_rdpViewItem->width() * cmpDpr));
		int winH = static_cast<int>(std::round(g_rdpViewItem->height() * cmpDpr));
		uint32_t alignedWinW = (static_cast<uint32_t>(std::max(winW, 0)) + 3) & ~3u;
		uint32_t alignedWinH = (static_cast<uint32_t>(std::max(winH, 0)) + 3) & ~3u;
		if (alignedWinW >= 640 && alignedWinH >= 480 &&
		    (newW != alignedWinW || newH != alignedWinH))
		{
			qf::log::warn("rdp/resize/dbg",
			    "GFX_RESET done but RDP {}x{} != window aligned {}x{} (window={}x{}) "
			    "— display may be out of sync, waiting for next resize trigger",
			    newW, newH, alignedWinW, alignedWinH, winW, winH);
		}
		else if (alignedWinW >= 640 && alignedWinH >= 480)
		{
			// RDP {}x{} matches window aligned {}x{} (window={}x{}) — no action needed
		}
	}

	return TRUE;
}

static BOOL noop_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmap)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(bitmap);
	return TRUE;
}

static BOOL noop_palette_update(rdpContext* context, const PALETTE_UPDATE* palette)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(palette);
	return TRUE;
}

static BOOL noop_play_sound(rdpContext* context, const PLAY_SOUND_UPDATE* play_sound)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(play_sound);
	return TRUE;
}

static BOOL noop_keyboard_set_indicators(rdpContext* context, UINT16 led_flags)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(led_flags);
	return TRUE;
}

static BOOL noop_keyboard_set_ime_status(rdpContext* context, UINT16 imeId, UINT32 imeState,
                                         UINT32 imeConvMode)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(imeId);
	WINPR_UNUSED(imeState);
	WINPR_UNUSED(imeConvMode);
	return TRUE;
}

static void qf_copy_file_descriptor_name(FILEDESCRIPTORW* descriptor, const QString& displayName)
{
	if (!descriptor)
		return;

	const qsizetype maxChars = static_cast<qsizetype>(ARRAYSIZE(descriptor->cFileName) - 1);
	const QString clippedName = displayName.left(static_cast<int>(maxChars));
	const ushort* utf16 = clippedName.utf16();

	for (qsizetype i = 0; i < clippedName.size(); ++i)
		descriptor->cFileName[i] = static_cast<WCHAR>(utf16[i]);
	descriptor->cFileName[clippedName.size()] = 0;
}


UINT qf_CliprdrServerFormatListCallBack(CliprdrClientContext* context,
                                        const CLIPRDR_FORMAT_LIST* formatList)
{
	if (!g_client->clipboard_format_from_remote_.empty())
		g_client->clipboard_format_from_remote_.clear();

	qf::log::info("cliprdr/format-list", "server advertised {} format(s)", formatList->numFormats);
	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const char* name = formatList->formats[i].formatName ? formatList->formats[i].formatName : "";
		g_client->clipboard_format_from_remote_[formatList->formats[i].formatId] = name;
	}

	auto requestRemoteFormat = [&](UINT32 formatId, const char* formatName)
	{
		CLIPRDR_FORMAT_DATA_REQUEST req = {};
		req.requestedFormatId = formatId;

		g_client->requested_remote_format_id_ = formatId;
		g_client->requested_remote_format_name = formatName ? formatName : "";

		context->ClientFormatDataRequest(context, &req);
	};

	if (g_client->clipboard_format_from_remote_.contains(CF_UNICODETEXT))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_UNICODETEXT");
		requestRemoteFormat(CF_UNICODETEXT, nullptr);
		return CHANNEL_RC_OK;
	}

	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const CLIPRDR_FORMAT* format = &formatList->formats[i];
		const char* name = format->formatName;
		if (name && !strcmp(name, "PNG"))
		{
			qf::log::info("cliprdr/format-select", "request remote PNG id={}", format->formatId);
			requestRemoteFormat(format->formatId, name);
			return CHANNEL_RC_OK;
		}

		if (name && !strcmp(name, qf::CLIPBOARD_FORMAT_FILE_NAME))
		{
			qf::log::info("cliprdr/format-select", "request remote FileGroupDescriptorW id={}",
			              format->formatId);
			requestRemoteFormat(format->formatId, qf::CLIPBOARD_FORMAT_FILE_NAME);
			return CHANNEL_RC_OK;
		}
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIBV5))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIBV5");
		requestRemoteFormat(CF_DIBV5, nullptr);
		return CHANNEL_RC_OK;
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIB))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIB");
		requestRemoteFormat(CF_DIB, nullptr);
		return CHANNEL_RC_OK;
	}

	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	if (!context || !formatDataResponse)
	{
		qf::log::warn("cliprdr/data-response", "invalid clipboard data response");
		return ERROR_INVALID_PARAMETER;
	}

	if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK)
	{
		qf::log::warn("cliprdr/data-response", "remote request failed formatId={} name={}",
		             g_client->requested_remote_format_id_, g_client->requested_remote_format_name);
		return CHANNEL_RC_OK;
	}

	const UINT32 requestedFormatId = g_client->requested_remote_format_id_;

	if (g_client->requested_remote_format_name == qf::CLIPBOARD_FORMAT_FILE_NAME)
	{
		UINT32 fileCount = 0;
		FILEDESCRIPTORW* fds = NULL;

		UINT32 error = cliprdr_parse_file_list(formatDataResponse->requestedFormatData,
		                                       formatDataResponse->common.dataLen,
		                                       &fds, &fileCount);
		if (error == CHANNEL_RC_OK)
		{
			std::vector<qf::clipboard_info_file_t> remoteFiles;
			remoteFiles.reserve(fileCount);
			for (UINT32 i = 0; i < fileCount; ++i)
			{
				FILEDESCRIPTORW* descriptor = &fds[i];
				const QString displayName =
				    QString::fromUtf16(reinterpret_cast<const char16_t*>(descriptor->cFileName));
				const uint64_t fileTotal =
				    (uint64_t(descriptor->nFileSizeHigh) << 32) | descriptor->nFileSizeLow;
				const DWORD attribute = descriptor->dwFileAttributes;
				const bool isDirectory = (attribute & FILE_ATTRIBUTE_DIRECTORY) != 0;

				qf::clipboard_info_file_t fileInfo;
				fileInfo.display_name_ = displayName;
				fileInfo.local_path_ = displayName;
				fileInfo.total_ = fileTotal;
				fileInfo.is_directory_ = isDirectory;
				remoteFiles.push_back(fileInfo);
			}

			if (g_clipboard_entry)
				g_clipboard_entry->enqueue_remote_file_list(context, remoteFiles);
		}
		else
		{
			qf::log::warn("cliprdr/file-list", "failed to parse remote file list error={}", error);
		}
		free(fds);
	}
	else
	{
		QByteArray clipboardData(reinterpret_cast<const char*>(formatDataResponse->requestedFormatData),
								static_cast<qsizetype>(formatDataResponse->common.dataLen));
		const QString requestedFormatName = QString::fromStdString(g_client->requested_remote_format_name);
		QMetaObject::invokeMethod(g_rdpViewItem, [clipboardData, requestedFormatId, requestedFormatName]() {
			g_rdpViewItem->updateClipboardDataFromRemote(clipboardData, requestedFormatId,
														requestedFormatName);
		}, Qt::QueuedConnection);

	}

	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatListResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataRequestCallBack(CliprdrClientContext* context,
                                               const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	if (!context || !formatDataRequest)
	{
		qf::log::warn("cliprdr/data-request", "invalid clipboard data request");
		return ERROR_INVALID_PARAMETER;
	}

	const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData();
	if (!mimeData)
	{
		qf::log::warn("cliprdr/data-request", "no local clipboard data available");
		return CHANNEL_RC_OK;
	}

	qf::log::info("cliprdr/data-request", "server requested formatId={}",
	              formatDataRequest->requestedFormatId);

	auto RemoteFormatDataResponse = [&](const BYTE* rawData, UINT32 dataLen) {
		CLIPRDR_FORMAT_DATA_RESPONSE req = {};
		req.common.msgFlags = CB_RESPONSE_OK;
		req.common.dataLen = dataLen;
		req.requestedFormatData = rawData;

		context->ClientFormatDataResponse(context, &req);
	};

	auto RemoteFormatDataFail = [&]() {
		CLIPRDR_FORMAT_DATA_RESPONSE req = {};
		req.common.msgFlags = CB_RESPONSE_FAIL;
		req.common.dataLen = 0;
		req.requestedFormatData = nullptr;

		context->ClientFormatDataResponse(context, &req);
		qf::log::warn("cliprdr/data-request", "unsupported formatId={}",
		             formatDataRequest->requestedFormatId);
	};

	if (mimeData->hasText() && formatDataRequest->requestedFormatId == CF_UNICODETEXT)
	{
		const QString text = mimeData->text();
		const char16_t* rawData = reinterpret_cast<const char16_t*>(text.utf16());
		UINT32 dataLen = (std::char_traits<char16_t>::length(rawData) + 1) * sizeof(char16_t); // 16bit char
		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(rawData), dataLen);
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == CF_DIB)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray bmp;
		QBuffer buffer(&bmp);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "BMP"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as DIB");
			return CHANNEL_RC_OK;
		}

		QByteArray dib = bmp.mid(14);	// skip 14 bytes of BMP header

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(dib.constData()),
		                         static_cast<UINT32>(dib.size()));
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_PNG)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray pngData;
		QBuffer buffer(&pngData);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "PNG"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as PNG");
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(pngData.constData()),
		                         static_cast<UINT32>(pngData.size()));
	}
	else if (mimeData->hasUrls() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_FILE)
	{
		std::vector<qf::clipboard_info_file_t> filesCopy;
		{
			std::lock_guard<std::mutex> lock(g_client->clipboard_info_files_mutex_);
			filesCopy = g_client->clipboard_info_files_;
		}

		const size_t fileCount = filesCopy.size();
		if (fileCount == 0)
		{
			qf::log::warn("cliprdr/file-list", "no local file list for FileGroupDescriptorW request");
			RemoteFormatDataFail();
			return CHANNEL_RC_OK;
		}

		std::vector<FILEDESCRIPTORW> fds(fileCount);
		for (size_t i = 0; i < fileCount; ++i)
		{
			const auto& file_info = filesCopy[i];
			const UINT64 fileSize = file_info.is_directory_ ? 0 : file_info.total_;
			fds[i].nFileSizeLow = static_cast<DWORD>(fileSize & 0xFFFFFFFFULL);
			fds[i].nFileSizeHigh = static_cast<DWORD>((fileSize >> 32) & 0xFFFFFFFFULL);
			fds[i].dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
			fds[i].dwFileAttributes = file_info.is_directory_ ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			qf_copy_file_descriptor_name(&fds[i], file_info.display_name_);
		}

		UINT32 flags = CB_STREAM_FILECLIP_ENABLED;
		if (g_client->cliprdr_file_context_)
		{
			const UINT32 remoteFlags = cliprdr_file_context_remote_get_flags(g_client->cliprdr_file_context_);
			if (remoteFlags & CB_STREAM_FILECLIP_ENABLED)
				flags = remoteFlags;
		}

		BYTE* serialize_data = nullptr;
		UINT32 serialize_data_size = 0;
		const UINT error = cliprdr_serialize_file_list_ex(flags, fds.data(), static_cast<UINT32>(fileCount),
		                                                  &serialize_data, &serialize_data_size);
		if (error || !serialize_data || serialize_data_size == 0)
		{
			qf::log::warn("cliprdr/file-list", "failed to serialize file list error={}", error);
			free(serialize_data);
			RemoteFormatDataFail();
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(serialize_data, serialize_data_size);
		qf::log::info("cliprdr/file-list", "sent FileGroupDescriptorW files={} bytes={}", fileCount,
		              serialize_data_size);
		free(serialize_data);
	}
	else
	{
		RemoteFormatDataFail();
	}

	return CHANNEL_RC_OK;
}
UINT qf_CliprdrMonitorReadyCallback(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY* monitorReady)
{
    g_client->cliprdr_client_context_ = context;
    qf::log::info("cliprdr/monitor", "monitor ready");

	CLIPRDR_CAPABILITIES capabilities = {};
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet = {};
	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCapabilitySet);
	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;
	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS;

	UINT rc = context->ClientCapabilities(context, &capabilities);
	if (rc != CHANNEL_RC_OK)
		return rc;

	CLIPRDR_FORMAT_LIST formatList = {};
	return context->ClientFormatList(context, &formatList);
}

UINT qf_ServerFileContentsRequest(CliprdrClientContext* context, 
	                                  const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	if (!g_clipboard_entry)
		return ERROR_INVALID_PARAMETER;
	return g_clipboard_entry->server_file_contents_request(context, request);
}

UINT qf_ServerFileContentsResponse(CliprdrClientContext* context,
                                   const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	if (!g_clipboard_entry)
		return ERROR_INVALID_PARAMETER;
	return g_clipboard_entry->server_file_contents_response(context, response);
}


void qt_clipboard_channel_init(CliprdrClientContext* clipboard)
{
	if (!clipboard)
	{
		qf::log::error("cliprdr/init", "clipboard channel init failed: null context");
		return;
	}

	g_client->clipboard_system_ = ClipboardCreate();
	if (!g_client->clipboard_system_)
	{
		qf::log::error("cliprdr/init", "WinPR clipboard system init failed");
		return;
	}

	if (!g_client->cliprdr_file_context_)
		g_client->cliprdr_file_context_ = cliprdr_file_context_new(g_client.get());
	if (!g_client->cliprdr_file_context_)
	{
		qf::log::error("cliprdr/init", "file context allocation failed");
		return;
	}

    clipboard->MonitorReady = qf_CliprdrMonitorReadyCallback;

	clipboard->ServerFormatList = qf_CliprdrServerFormatListCallBack;
	clipboard->ServerFormatListResponse = qf_CliprdrServerFormatListResponseCallBack;

    clipboard->ServerFormatDataRequest = qf_CliprdrServerFormatDataRequestCallBack;
    clipboard->ServerFormatDataResponse = qf_CliprdrServerFormatDataResponseCallBack;

	if(!cliprdr_file_context_init(g_client->cliprdr_file_context_, clipboard))
	{
		qf::log::error("cliprdr/init", "file context init failed");
		return;
	}

	if (!g_clipboard_entry)
		g_clipboard_entry = std::make_unique<qf::clipboard_entry>(g_client);
	clipboard->ServerFileContentsRequest = qf_ServerFileContentsRequest;
	clipboard->ServerFileContentsResponse = qf_ServerFileContentsResponse;
}

void qf_channel_connected_callback(void* context, const ChannelConnectedEventArgs* event)
{
	qf::log::info("channel/connect", "connected name={} interface={}", event->name, fmt::ptr(event->pInterface));

	/* Forward to FreeRDP common handler for standard channels (GFX, disp, etc.) */
	freerdp_client_OnChannelConnectedEventHandler(context, event);

	if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME))
	{
		qf::log::info("cliprdr/init", "initializing clipboard channel");
		qt_clipboard_channel_init(static_cast<CliprdrClientContext*>(event->pInterface));
	}

	if (g_gfxContext == nullptr &&
	    strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		g_gfxContext = static_cast<RdpgfxClientContext*>(event->pInterface);
		qf::log::info("gfx/init", "GFX context saved");
	}

	/* rdpecam control channel (RDCamera_Device_Enumerator).
	 * Note: the event->name is the protocol name, NOT RDPECAM_DVC_CHANNEL_NAME.
	 * Use substring matching to be robust against any prefix/suffix differences. */
	if (strstr(event->name, "RDCamera") != nullptr)
	{
		qf::log::info("rdpecam/init", "camera channel connected (name={})", event->name);
	}

	/* Save disp context and trigger deferred resize on the main thread. */
	if (strcmp(event->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		g_dispContext = static_cast<DispClientContext*>(event->pInterface);
		qf::log::warn("disp/init", ">>> display control context saved <<<");
		if (g_rdpViewItem)
		{
			QMetaObject::invokeMethod(
			    g_rdpViewItem,
			    []()
			    {
				    if (!g_dispContext || !g_rdpViewItem || !g_instance)
					    return;
				    uint32_t w = static_cast<uint32_t>(g_rdpViewItem->width());
				    uint32_t h = static_cast<uint32_t>(g_rdpViewItem->height());
				    uint32_t winDiPs_w = w, winDiPs_h = h;
				    /* HiDPI：将逻辑像素乘以 DPR 转为物理像素 */
				    double dpr = 1.0;
				    if (auto* win = g_rdpViewItem->window())
					    dpr = win->devicePixelRatio();
				    w = static_cast<uint32_t>(std::round(w * dpr));
				    h = static_cast<uint32_t>(std::round(h * dpr));
				    uint32_t wa = (w + 3) & ~3u;
				    uint32_t ha = (h + 3) & ~3u;
				    uint32_t currentW = freerdp_settings_get_uint32(
				        g_instance->context->settings, FreeRDP_DesktopWidth);
				    uint32_t currentH = freerdp_settings_get_uint32(
				        g_instance->context->settings, FreeRDP_DesktopHeight);
				    qf::log::info("rdp/resize",
				        "disp init: DIPs={}x{} dpr={} phys={}x{} aligned={}x{} currentRdp={}x{}",
				        winDiPs_w, winDiPs_h, dpr, w, h, wa, ha, currentW, currentH);
				    if (w > 0 && h > 0 && (wa != currentW || ha != currentH))
				    {
					    qf::log::warn("rdp/resize", ">>> disp resize {}x{} -> {}x{}", currentW,
					                  currentH, wa, ha);
					    DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};
					    layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
					    layout.Width = wa;
					    layout.Height = ha;
					    layout.Orientation = ORIENTATION_LANDSCAPE;
					    layout.DesktopScaleFactor = 100;
					    layout.DeviceScaleFactor = 100;
					    UINT error =
					        g_dispContext->SendMonitorLayout(g_dispContext, 1, &layout);
					    if (error != CHANNEL_RC_OK)
						    qf::log::error("rdp/resize",
						                   "SendMonitorLayout failed error={}", error);
				    }
				    else
				    {
					    qf::log::warn("rdp/resize", ">>> no resize needed ({}x{})", currentW,
					                  currentH);
				    }
			    },
			    Qt::QueuedConnection);
		}
	}
}

void qf_channel_disconnected_callback(void* context, const ChannelDisconnectedEventArgs* event)
{
	freerdp_client_OnChannelDisconnectedEventHandler(context, event);

	/* rdpecam disconnect logging */
	if (strstr(event->name, "RDCamera") != nullptr)
	{
		qf::log::info("rdpecam/cleanup", "camera channel disconnected (name={})", event->name);
	}
}

static BOOL qf_load_cliprdr_addin(freerdp* instance)
{
	if (!instance || !instance->context || !instance->context->channels || !instance->context->settings)
		return FALSE;

	rdpSettings* settings = instance->context->settings;
	const char* const args[] = { CLIPRDR_SVC_CHANNEL_NAME };

	if (!freerdp_static_channel_collection_find(settings, CLIPRDR_SVC_CHANNEL_NAME))
	{
		if (!freerdp_client_add_static_channel(settings, 1, args))
		{
			qf::log::error("cliprdr/load", "failed to add static channel");
			return FALSE;
		}
	}

	ADDIN_ARGV* cliprdrArgs = freerdp_static_channel_collection_find(
	    settings, CLIPRDR_SVC_CHANNEL_NAME);
	if (!cliprdrArgs)
	{
		qf::log::error("cliprdr/load", "static channel args missing after add");
		return FALSE;
	}

	const int rc = freerdp_channels_client_load_ex(instance->context->channels, settings,
	                                               cliprdr_VirtualChannelEntryEx, cliprdrArgs);
	if (rc != CHANNEL_RC_OK)
	{
		qf::log::error("cliprdr/load", "failed to load channel add-in rc={}", rc);
		return FALSE;
	}

	qf::log::info("cliprdr/load", "channel add-in loaded");
	return TRUE;
}

/* Forward declaration - exported from libfreerdp-client3.so */
extern "C" BOOL freerdp_client_load_addins(rdpChannels* channels, rdpSettings* settings);

static BOOL my_load_channels(freerdp* instance)
{
	if (!instance || !instance->context || !instance->context->channels || !instance->context->settings)
		return FALSE;

	rdpSettings* settings = instance->context->settings;

	/* Delegate to FreeRDP's built-in addin loader */
	if (!freerdp_client_load_addins(instance->context->channels, settings))
	{
		qf::log::error("channels/load", "freerdp_client_load_addins failed");
		return FALSE;
	}

	return TRUE;
}

// 1. 预连接回调函数，在这里配置所有连接参数
static BOOL my_pre_connect(freerdp* instance)
{
	rdpSettings* settings = instance->context->settings;

	qf::log::info("rdp/pre-connect", "configuring connection settings");

	// 清除之前连接遗留的设备/动态通道配置，防止重连时重复注册
	freerdp_device_collection_free(settings);
	freerdp_dynamic_channel_collection_free(settings);

	// FreeRDP defaults to enabling clipboard; disable before CLI parsing
	freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, FALSE);

	// 解析命令行参数（仅首次连接时执行，重连时跳过）
	if (!g_cli_parsed && g_cli_argc > 1)
	{
		int status = freerdp_client_settings_parse_command_line(
			settings, g_cli_argc, g_cli_argv, FALSE);
		if (status < 0)
		{
			qf::log::error("rdp/pre-connect",
			               "command-line parsing failed (status=%d)", status);
			freerdp_client_settings_command_line_status_print(
				settings, status, g_cli_argc, g_cli_argv);
			return FALSE;
		}
		qf::log::info("rdp/pre-connect",
		              "command-line arguments parsed successfully");
		g_cli_parsed = true;
		/* Save resolution if provided via /w: /h: or .rdp file */
		g_cli_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
		g_cli_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
		if (g_cli_width > 0 && g_cli_height > 0)
			qf::log::warn("rdp/pre-connect",
			    ">>> CLI/.rdp provided resolution: {}x{} <<<",
			    g_cli_width, g_cli_height);
		/* Always ignore .rdp file resolution — use screen/window size for correct display */
		if (g_cli_width > 0 || g_cli_height > 0)
		{
			qf::log::warn("rdp/pre-connect",
			    ">>> ignoring .rdp resolution, will use window size <<<");
			g_cli_width = 0;
			g_cli_height = 0;
		}

		// 检测 CLI 或 .rdp 文件是否启用了 USB 重定向。
		ADDIN_ARGV* usbDvc =
		    freerdp_dynamic_channel_collection_find(settings, URBDRC_CHANNEL_NAME);
		g_usb_cli_enabled = (usbDvc != nullptr);
		if (g_usb_cli_enabled)
		{
			qf::log::info("rdp/pre-connect",
			              "USB redirect enabled by CLI/.rdp file");
			// 保存 USB 值用于重连恢复
			g_saved_usb_value.clear();
			for (int i = 1; i < g_cli_argc; i++)
			{
				if (g_cli_argv[i] &&
				    strncmp(g_cli_argv[i], "/usb:", 5) == 0)
				{
					g_saved_usb_value = g_cli_argv[i] + 5;
					break;
				}
			}
			if (g_saved_usb_value.empty())
				g_saved_usb_value = "device:*";
		}

		// 保存 /drive: 参数用于重连时恢复
		g_saved_drive_args.clear();
		for (int i = 1; i < g_cli_argc; i++)
		{
			if (g_cli_argv[i] &&
			    strncmp(g_cli_argv[i], "/drive:", 7) == 0)
			{
				g_saved_drive_args.push_back(g_cli_argv[i] + 7);
			}
		}

		// 处理 /drives 简写 — 枚举所有本地固定磁盘并添加到重定向列表
		for (int i = 1; i < g_cli_argc; i++)
		{
			if (g_cli_argv[i] && strcmp(g_cli_argv[i], "/drives") == 0)
			{
				DWORD logicalDrives = GetLogicalDrives();
				qf::log::warn("rdp/pre-connect",
				              "/drives: enumerating local fixed drives (drives_bitmask=0x{:08x})",
				              logicalDrives);
				for (int d = 0; d < 26; d++)
				{
					if (logicalDrives & (1 << d))
					{
						char letter = 'A' + d;
						char root[] = {letter, ':', '\\', '\0'};
						if (GetDriveTypeA(root) == DRIVE_FIXED)
						{
							char arg[64] = {};
							snprintf(arg, sizeof(arg), "%c,%c:\\", letter, letter);
							g_saved_drive_args.push_back(arg);
							qf::log::warn("rdp/pre-connect",
							              "/drives: added {}", arg);
						}
					}
				}
				break;
			}
		}

		// FreeRDP 的 CLI 解析器处理 /clipboard:direction-to:* 和 /clipboard:files-to:*
		// 等子选项时，只会更新 FreeRDP_ClipboardFeatureMask，但不会设置
		// FreeRDP_RedirectClipboard = TRUE。
		if (!freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard))
		{
			for (int i = 1; i < g_cli_argc; i++)
			{
				if (g_cli_argv[i] &&
				    strncmp(g_cli_argv[i], "/clipboard:", 11) == 0)
				{
					freerdp_settings_set_bool(
					    settings, FreeRDP_RedirectClipboard, TRUE);
					qf::log::info("rdp/pre-connect",
					              "clipboard re-enabled for /clipboard: sub-option");
					break;
				}
			}
		}
	}

	// ==== DEBUG: TCP resolve diagnostics ====
	{
		const char* hostname = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
		UINT32 port = freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
		if (hostname)
		{
			qf::log::warn("rdp/debug/dns",
			    "Hostname='{}' port={}", hostname, port);

			// Helper lambda: convert gai error to string (MSVC gai_strerror returns WCHAR*)
			auto gai_err_str = [](int rc) -> std::string {
				if (rc == 0) return "OK";
				const wchar_t* w = gai_strerrorW(rc);
				if (!w) return "unknown";
				char buf[256] = {};
				WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf), nullptr, nullptr);
				return buf;
			};

			// Test 1: original AF_UNSPEC, no flags
			{
				struct addrinfo hints = {};
				hints.ai_flags = 0;
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				sprintf_s(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test1 (AF_UNSPEC): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 2: AF_INET
			{
				struct addrinfo hints = {};
				hints.ai_flags = 0;
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				sprintf_s(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test2 (AF_INET): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 3: AF_INET | AI_NUMERICHOST
			{
				struct addrinfo hints = {};
				hints.ai_flags = AI_NUMERICHOST;
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				sprintf_s(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test3 (AF_INET|AI_NUMERICHOST): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 4: AF_UNSPEC | AI_NUMERICHOST
			{
				struct addrinfo hints = {};
				hints.ai_flags = AI_NUMERICHOST;
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				sprintf_s(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test4 (AF_UNSPEC|AI_NUMERICHOST): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}
		}
	}

	// 磁盘重定向 — 从保存的 CLI 参数恢复（首次连接或重连均执行）
	if (!g_saved_drive_args.empty() &&
	    !freerdp_device_collection_find_type(settings, RDPDR_DTYP_FILESYSTEM))
	{
		for (const auto& arg : g_saved_drive_args)
		{
			auto comma = arg.find(',');
			if (comma == std::string::npos || comma == 0 || comma == arg.size() - 1)
			{
				qf::log::warn("rdp/pre-connect",
				              "invalid saved drive arg: {}", arg);
				continue;
			}
			std::string name = arg.substr(0, comma);
			std::string path = arg.substr(comma + 1);
			const char* args[] = {"drive", name.c_str(), path.c_str(), nullptr};
			if (!freerdp_client_add_device_channel(settings, 3, args))
				qf::log::error("rdp/pre-connect",
				               "restore drive {} -> {} failed", name, path);
			else
				qf::log::info("rdp/pre-connect",
				              "restored drive {} -> {}", name, path);
		}
	}

	// 分辨率：使用 QML 窗口布局完成后的实际 Item 尺寸
	{
		uint32_t actualW = g_cli_width;
		uint32_t actualH = g_cli_height;
		if (actualW == 0 || actualH == 0)
		{
			if (auto* win = g_rdpViewItem->window())
			{
				actualW = static_cast<uint32_t>(std::round(
				    g_rdpViewItem->width() * win->devicePixelRatio()));
				actualH = static_cast<uint32_t>(std::round(
				    g_rdpViewItem->height() * win->devicePixelRatio()));
				qf::log::warn("rdp/pre-connect",
				    ">>> pre-connect: item={}x{} dpr={} phys={}x{} <<<",
				    g_rdpViewItem->width(), g_rdpViewItem->height(),
				    win->devicePixelRatio(), actualW, actualH);
			}
		}
		if (actualW == 0 || actualH == 0)
		{
			actualW = g_client->view_width_;
			actualH = g_client->view_height_;
		}
		if (actualW == 0 || actualH == 0)
		{
			actualW = 1024;
			actualH = 768;
		}
		qf::log::warn("rdp/display", ">>> Sending DesktopWidth={} DesktopHeight={} to RDP server <<<", actualW, actualH);
		freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, actualW);
		freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, actualH);

		/* Set monitor layout so the server uses our resolution during GFX init */
		{
			rdpMonitor monitors[1] = {};
			monitors[0].x          = 0;
			monitors[0].y          = 0;
			monitors[0].width      = static_cast<INT32>(actualW);
			monitors[0].height     = static_cast<INT32>(actualH);
			monitors[0].is_primary = TRUE;
			monitors[0].attributes.orientation        = ORIENTATION_LANDSCAPE;
			monitors[0].attributes.desktopScaleFactor  = 100;
			monitors[0].attributes.deviceScaleFactor   = 100;
			freerdp_settings_set_monitor_def_array_sorted(settings, monitors, 1);
			qf::log::warn("rdp/display",
			    ">>> Set monitor layout: {}x{} is_primary=1 <<<",
			    monitors[0].width, monitors[0].height);
		}
	}
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

	// 跳过证书强校验
	freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

	// 允许本地图形解码。
	freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE);

	// 启用标准 RDP 压缩。
	freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, TRUE);

	freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, TRUE);


	// TCP 连接超时（单位毫秒，默认 15000 = 15 秒）
	freerdp_settings_set_uint32(settings, FreeRDP_TcpConnectTimeout, 15000);

	freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, TRUE);

	// USB Redirection
	if (g_usb_cli_enabled)
	{
		if (g_usbManager)
		{
			auto ids = g_usbManager->selectedDeviceIds();
			if (!ids.empty())
			{
				qf::log::info("rdp/pre-connect",
				              "toolbar has {} device(s) selected, adding via toolbar",
				              ids.size());
				for (const auto& [vid, pid] : ids)
				{
					char devId[32];
					snprintf(devId, sizeof(devId), "id:%04x:%04x", vid, pid);
					const char* usb_args[] = {URBDRC_CHANNEL_NAME, devId, nullptr};
					if (!freerdp_client_add_dynamic_channel(settings, 2, usb_args))
						qf::log::warn("rdp/pre-connect", "USB redirect failed for {}", devId);
					else
						qf::log::info("rdp/pre-connect", "USB redirect enabled for {}", devId);
				}
			}
			else
			{
				ADDIN_ARGV* existing =
				    freerdp_dynamic_channel_collection_find(settings, URBDRC_CHANNEL_NAME);
				if (!existing && !g_saved_usb_value.empty())
				{
					qf::log::info("rdp/pre-connect",
					              "restoring USB redirect from saved value: {}",
					              g_saved_usb_value);
					size_t count = 0;
					char** ptr = CommandLineParseCommaSeparatedValuesEx(
					    URBDRC_CHANNEL_NAME, g_saved_usb_value.c_str(), &count);
					if (ptr)
					{
						freerdp_client_add_dynamic_channel(settings, count,
						                                   (const char* const*)ptr);
						CommandLineParserFree(ptr);
					}
				}
			}
		}
	}

	// 摄像头重定向（rdpecam）— 使用 WMF (Media Foundation) 后端
	{
		const char* camera_args[] = {RDPECAM_DVC_CHANNEL_NAME, "device:*", nullptr};
		if (!freerdp_client_add_dynamic_channel(settings, 2, camera_args))
			qf::log::warn("rdp/pre-connect", "camera redirect failed");
		else
			qf::log::info("rdp/pre-connect", "camera redirect enabled (all devices)");
	}

	// 麦克风重定向（audin）— 使用 WinMM 后端（仅在有麦克风设备时启用）
	{
		UINT numDevs = waveInGetNumDevs();
		if (numDevs == 0)
		{
			qf::log::info("rdp/pre-connect",
			    "no microphone device found, skipping audin");
			freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE);
		}
		else
		{
			const char* mic_args[] = {AUDIN_CHANNEL_NAME, "sys:winmm", nullptr};
			if (!freerdp_client_add_dynamic_channel(settings, 2, mic_args))
				qf::log::warn("rdp/pre-connect", "microphone redirect failed");
			else
				qf::log::info("rdp/pre-connect",
				    "microphone redirect enabled (WinMM, {} device(s))", numDevs);
			freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, TRUE);
		}
	}

	// GFX 图形管道
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_FrameAcknowledge, 2);
	freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);

	PubSub_SubscribeChannelConnected(instance->context->pubSub, qf_channel_connected_callback);
	PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                    qf_channel_disconnected_callback);

	qf::log::info("rdp/pre-connect", "configuration applied");
	return TRUE;
}

static BOOL my_post_connect(freerdp* instance)
{
	rdpUpdate* update = instance->context->update;

	update->BeginPaint = noop_begin_paint;
	update->EndPaint = noop_end_paint;
	update->DesktopResize = noop_desktop_resize;
	update->Palette = noop_palette_update;
	update->PlaySound = noop_play_sound;
	update->SetKeyboardIndicators = noop_keyboard_set_indicators;
	update->SetKeyboardImeStatus = noop_keyboard_set_ime_status;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
	{
		qf::log::error("rdp/post-connect", "gdi_init failed");
		return FALSE;
	}

		/* Keep FreeRDP's default GDI buffer (heap-allocated by gdi_init).
		 * No DMA-BUF -- frame data is copied to a staging buffer in
		 * updateGdiFrame() and uploaded via QSGSimpleTextureNode.
		 */
		{
			rdpGdi* gdi = instance->context->gdi;
			const uint32_t w = freerdp_settings_get_uint32(
			    instance->context->settings, FreeRDP_DesktopWidth);
			const uint32_t h = freerdp_settings_get_uint32(
			    instance->context->settings, FreeRDP_DesktopHeight);
			/* Resize staging buffer synchronously on RDP thread. */
			g_rdpViewItem->resizeStagingBuffer(w, h);
			/* Dispatch GUI-thread-only work via invokeMethod. */
			QMetaObject::invokeMethod(g_rdpViewItem, [=]() {
				g_rdpViewItem->notifyFrameResized();
			}, Qt::QueuedConnection);
		}
	/* Register RDP pointer (cursor) callbacks. */
	{
		rdpPointer pointer;
		memset(&pointer, 0, sizeof(pointer));
		pointer.size        = sizeof(pointer);
		pointer.New         = my_pointer_new;
		pointer.Free        = my_pointer_free;
		pointer.Set         = my_pointer_set;
		pointer.SetNull     = my_pointer_setnull;
		pointer.SetDefault  = my_pointer_setdefault;
		pointer.SetPosition = my_pointer_setposition;
		graphics_register_pointer(instance->context->graphics, &pointer);
		qf::log::info("rdp/pointer", "pointer callbacks registered");
	}

	g_rdpViewItem->setFreeRDP_context(instance->context);

	QMetaObject::invokeMethod(g_rdpViewItem, []() {
		notify_window_resized();
	}, Qt::QueuedConnection);

	return TRUE;
}

void rdp_loop_thread()
{
	DWORD rc = 1;

	qf::log::info("rdp/session", "starting FreeRDP loop");

	// Register FreeRDP's built-in static addin provider, then wrap it
	// with our custom provider
	freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
	g_orig_addin_provider = freerdp_get_current_addin_provider();
	freerdp_register_addin_provider(qf_addin_provider, 0);
	g_instance = freerdp_new();
	if (!g_instance)
	{
		qf::log::error("rdp/session", "failed to allocate FreeRDP instance");
		return;
	}

	// 3. 注册回调
	g_instance->PreConnect = my_pre_connect;
	g_instance->PostConnect = my_post_connect;
	g_instance->LoadChannels = my_load_channels;

	// 4. 创建 RDP 上下文
	if (!freerdp_context_new(g_instance))
	{
		qf::log::error("rdp/session", "failed to allocate RDP context");
		goto fail;
	}

	// 5. 尝试连接（瞬态失败自动重试 3 次）
	qf::log::info("rdp/connect", "attempting to connect");

	{
		BOOL connected = FALSE;
		int attempt = 0;
		while (!connected && attempt < 3 && !g_stopped)
		{
			if (attempt > 0)
			{
				int delay = 500;
				qf::log::info("rdp/connect", "retry attempt {}/3 after {}ms",
				              attempt + 1, delay);
				Sleep(delay);
			}
			attempt++;
			connected = freerdp_connect(g_instance);
			if (connected)
			{
				qf::log::info("rdp/connect", "connected successfully");
				break;
			}
			DWORD freerdp_err = freerdp_get_last_error(g_instance->context);
			int wsa_err = WSAGetLastError();
			qf::log::error("rdp/connect", "connection failed: freerdp_error=0x{:08X} errno={} [{}] wsa_err={}",
			               freerdp_err, errno, strerror(errno), wsa_err);
		}

		if (!connected)
		{
			if (g_rdpViewItem)
			{
				QMetaObject::invokeMethod(g_rdpViewItem, "notifyDisconnected",
				                          Qt::QueuedConnection);
			}
		}
	}

	if (!freerdp_shall_disconnect_context(g_instance->context) && !g_stopped)
	{
		enum { STATE_CONNECTED, STATE_RECONNECTING, STATE_DISCONNECTED } state = STATE_CONNECTED;
		DWORD nCount = 0;
		DWORD status = 0;
		HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};

		while (state != STATE_DISCONNECTED && !g_stopped)
		{
			switch (state)
			{
				case STATE_CONNECTED:
				{
					nCount = freerdp_get_event_handles(g_instance->context, handles,
					                                   ARRAYSIZE(handles));

					if (nCount == 0)
					{
						state = STATE_RECONNECTING;
						break;
					}

					status = WaitForMultipleObjects(nCount, handles, FALSE, 100);

					if (status == WAIT_FAILED)
					{
						state = STATE_RECONNECTING;
						break;
					}

					// Check reconnect request before processing events
					if (g_reconnectRequested.exchange(false))
					{
						qf::log::info("rdp/reconnect",
						              "USB device changed, reconnecting...");
						state = STATE_RECONNECTING;
						break;
					}

					if (status == WAIT_TIMEOUT)
						{ break; }

					if (!freerdp_check_event_handles(g_instance->context))
					{
						state = STATE_RECONNECTING;
						break;
					}

					if (freerdp_shall_disconnect_context(g_instance->context))
						state = STATE_DISCONNECTED;
					break;
				}

				case STATE_RECONNECTING:
				{
					BOOL connected = FALSE;

					for (int attempt = 0; attempt < 3 && !g_stopped; attempt++)
					{
						freerdp_disconnect(g_instance);

						g_dispContext = nullptr;
						g_gfxContext = nullptr;
						g_clipboard_client_context = nullptr;
						g_client->cliprdr_client_context_ = nullptr;

						if (freerdp_connect(g_instance))
						{
							connected = TRUE;
							break;
						}

						for (int i = 0; i < 5 && !g_stopped; i++)
							Sleep(100);
					}

					state = connected ? STATE_CONNECTED : STATE_DISCONNECTED;
					break;
				}

				case STATE_DISCONNECTED:
					break;
			}
		}

		rc = freerdp_get_last_error(g_instance->context);

		qf::log::info("rdp/disconnect", "disconnecting");
		freerdp_disconnect(g_instance);

		if (g_usbManager)
			g_usbManager->clearSelection();

		if (g_rdpViewItem)
		{
			QMetaObject::invokeMethod(g_rdpViewItem, "notifyDisconnected",
			                          Qt::QueuedConnection);
		}
	}

fail:
	qf::log::info("rdp/session", "test finished");
	freerdp_context_free(g_instance);
	freerdp_free(g_instance);

	qf::log::info("rdp/session", "FreeRDP instance freed rc={}", rc);
}

void stop()
{
	g_stopped = true;
	if (g_freerdp_thread)
	{
		g_freerdp_thread->join();
		g_freerdp_thread.reset();
	}
}

/* Debounce timer for live-resize. */
static QTimer* g_resize_debounce = nullptr;

void notify_window_resized()
{
	if (!g_dispContext || !g_instance || !g_rdpViewItem)
	{
		qf::log::warn("rdp/resize/dbg",
		    "notify_window_resized: SKIP — null guard (disp={} inst={} view={})",
		    fmt::ptr(g_dispContext), fmt::ptr(g_instance), fmt::ptr(g_rdpViewItem));
		return;
	}

	if (!g_resize_debounce)
	{
		g_resize_debounce = new QTimer();
		g_resize_debounce->setSingleShot(true);
		QObject::connect(g_resize_debounce, &QTimer::timeout,
		                 []()
		                 {
			                 if (!g_dispContext || !g_rdpViewItem || !g_instance)
			                 {
				                 qf::log::warn("rdp/resize/dbg",
				                     "timer fired but SKIP — null guard");
				                 return;
			                 }

			                 uint32_t w = static_cast<uint32_t>(g_rdpViewItem->width());
			                 uint32_t h = static_cast<uint32_t>(g_rdpViewItem->height());

			                 double dpr = 1.0;
			                 if (auto* win = g_rdpViewItem->window())
				                 dpr = win->devicePixelRatio();
			                 w = static_cast<uint32_t>(std::round(w * dpr));
			                 h = static_cast<uint32_t>(std::round(h * dpr));

			                 uint32_t wa = (w + 3) & ~3u;
			                 uint32_t ha = (h + 3) & ~3u;

			                 uint32_t currentW = freerdp_settings_get_uint32(
			                     g_instance->context->settings, FreeRDP_DesktopWidth);
			                 uint32_t currentH = freerdp_settings_get_uint32(
			                     g_instance->context->settings, FreeRDP_DesktopHeight);

			                 auto now = std::chrono::steady_clock::now();
			                 int pending = g_pending_resize_count.load(std::memory_order_acquire);
			                 uint32_t lastSentW = g_last_disp_w.load(std::memory_order_acquire);
			                 uint32_t lastSentH = g_last_disp_h.load(std::memory_order_acquire);

			                 if (wa < 640 || ha < 480)
			                 {
				                 return;
			                 }

			                 if (wa != currentW || ha != currentH)
			                 {
				                 if (pending > 0 && wa == lastSentW && ha == lastSentH)
				                 {
					                 return;
				                 }

				                 qf::log::warn("rdp/resize", ">>> disp resize {}x{} -> {}x{}",
				                               currentW, currentH, wa, ha);
				                 DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};
				                 layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
				                 layout.Width = wa;
				                 layout.Height = ha;
				                 layout.Orientation = ORIENTATION_LANDSCAPE;
				                 layout.DesktopScaleFactor = 100;
				                 layout.DeviceScaleFactor = 100;

				                 g_last_disp_send_ts = now;
				                 g_last_disp_w.store(wa, std::memory_order_release);
				                 g_last_disp_h.store(ha, std::memory_order_release);
				                 g_pending_resize_count.fetch_add(1, std::memory_order_acq_rel);

				                 UINT error = g_dispContext->SendMonitorLayout(g_dispContext, 1, &layout);
				                 if (error != CHANNEL_RC_OK)
				                     qf::log::error("rdp/resize",
				                         "SendMonitorLayout FAILED error={}", error);
			                 }
			                 else
				                 {
				                 }
		                 });
	}

	g_resize_debounce->start(300);
}

void start_rdp_connection()
{
	if (!g_rdpViewItem || !g_client)
		return;

	// 防止在 g_client 初始化后被重复调用
	static std::atomic<bool> s_started{false};
	if (s_started.exchange(true))
		return;

	ensure_blank_cursor();

	// Get primary screen resolution directly (always fullscreen mode)
	{
		auto* screen = QGuiApplication::primaryScreen();
		if (screen)
		{
			QSize screenSize = screen->size();
			double dpr = screen->devicePixelRatio();
			g_client->view_width_ = static_cast<uint32_t>(
			    std::round(screenSize.width() * dpr));
			g_client->view_height_ = static_cast<uint32_t>(
			    std::round(screenSize.height() * dpr));
			qf::log::warn("rdp/start",
			    ">>> screen resolution: {}x{} (dpr={}) <<<",
			    g_client->view_width_, g_client->view_height_, dpr);
		}
		else
		{
			g_client->view_width_  = 1024;
			g_client->view_height_ = 768;
		}
	}

	QTimer::singleShot(50, []()
	{
		// Verify window size matches screen (post-layout sanity check)
		if (auto* win = g_rdpViewItem->window())
		{
			uint32_t w = static_cast<uint32_t>(std::round(
			    g_rdpViewItem->width() * win->devicePixelRatio()));
			uint32_t h = static_cast<uint32_t>(std::round(
			    g_rdpViewItem->height() * win->devicePixelRatio()));
			qf::log::warn("rdp/start",
			    ">>> deferred check: item={}x{} dpr={} phys={}x{} <<<",
			    g_rdpViewItem->width(), g_rdpViewItem->height(),
			    win->devicePixelRatio(), w, h);
			if (w >= 640 && h >= 480 && g_client->view_width_ != w)
			{
				qf::log::warn("rdp/start",
				    ">>> updating view size to match window: {}x{} <<<", w, h);
				g_client->view_width_  = w;
				g_client->view_height_ = h;
			}
		}
		qf::log::warn("rdp/start",
		    ">>> starting connection with window size {}x{}",
		    g_client->view_width_, g_client->view_height_);
		g_freerdp_thread = std::make_unique<std::thread>(rdp_loop_thread);
	});
}

int main(int argc, char* argv[])
{
	// 自动设置 OpenSSL 环境变量，确保 legacy.dll 和 openssl.cnf 被正确加载
	// 注意：必须使用 _putenv_s（C 运行时）而非 SetEnvironmentVariableA（Win32 API），
	// 因为 OpenSSL 内部使用 getenv() 读取 OPENSSL_MODULES，而 Win32 和 C 运行时
	// 在 Windows 上维护各自独立的环境缓冲区。
	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0)
	{
		std::string exeDir = exePath;
		auto pos = exeDir.find_last_of('\\');
		if (pos != std::string::npos)
		{
			exeDir = exeDir.substr(0, pos);
			_putenv_s("OPENSSL_MODULES", exeDir.c_str());
			SetEnvironmentVariableA("OPENSSL_MODULES", exeDir.c_str());

			std::string confPath = exeDir + "\\openssl.cnf";
			_putenv_s("OPENSSL_CONF", confPath.c_str());
			SetEnvironmentVariableA("OPENSSL_CONF", confPath.c_str());
		}
	}

	// Store command-line arguments for later use in rdp_loop_thread
	g_cli_argc = argc;
	g_cli_argv = argv;

	// Always fullscreen — no longer depends on /f flag

	// Initialize Winsock (required before any socket operations)
	WSADATA wsaData;
	int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaErr != 0)
	{
		qf::log::error("main", "WSAStartup failed: {}", wsaErr);
		return 1;
	}

	qf::log::init();

	// Disable Qt Quick pipeline cache to avoid creating %LOCALAPPDATA%\qf-client\cache\
	qputenv("QSG_PIPELINE_CACHE", "0");

	QGuiApplication app(argc, argv);

	// Set application window icon (from qrc resources.qrc)
	QIcon appIcon(QStringLiteral(":/app.ico"));
	app.setWindowIcon(appIcon);

	QQmlApplicationEngine engine;

	// Create USBManager and expose to QML
	g_usbManager = std::make_unique<USBManager>();
	engine.rootContext()->setContextProperty("usbManager", g_usbManager.get());

	// Always use fullscreen window mode — expose to QML
	engine.rootContext()->setContextProperty("cliFullscreen", QVariant(true));

	// When USB selection is applied, trigger a reconnect
	QObject::connect(g_usbManager.get(), &USBManager::reconnectRequested, []() {
		g_reconnectRequested = true;
	});

	// QML 文件路径: qrc:/MyTestApp/src/main.qml（因 CMake QML_FILES 使用相对路径 src/main.qml）
	const QUrl url(QStringLiteral("qrc:/MyTestApp/src/main.qml"));

	QObject::connect(
	    &engine, &QQmlApplicationEngine::objectCreated, &app,
	    [url](QObject* obj, const QUrl& objUrl)
	    {
		    if (!obj && url == objUrl)
		    {
			    QCoreApplication::exit(-1);
		    }
	    },
	    Qt::QueuedConnection);

	engine.load(url);

	if (engine.rootObjects().isEmpty())
	{
		qWarning("Failed to load QML file.");
		stop();
		return -1;
	}

	QObject* root = engine.rootObjects().first();
	g_rdpViewItem = root->findChild<RdpViewItem*>("rdpViewItem");
	if (!g_rdpViewItem)
	{
		qWarning("Failed to find RdpViewItem in QML.");
		stop();
		return -1;
	}

	g_client = std::make_shared<qf::client_t>();
	g_rdpViewItem->set_qfclient_context(g_client);
	g_client->rdpViewItem = g_rdpViewItem;

	// 所有初始化完成后，从 C++ 侧启动 RDP 连接（避免 QML Component.onCompleted
	// 在 engine.load() 期间触发时 g_client 仍为 nullptr 的问题）
	start_rdp_connection();

	int rt = app.exec();

	stop();

	return rt;
}