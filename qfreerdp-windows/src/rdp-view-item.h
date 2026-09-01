#pragma once

#include <QObject>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>
#include <QRunnable>
#include <QMutex>
#include <QImage>
#include <QtQml/qqmlregistration.h>
#include <qeventloop.h>
#include <qnamespace.h>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QClipboard>
#include <QMimeData>
#include <QByteArray>
#include <QtEndian>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QCursor>
#include <winpr/user.h>

/* QRhiTexture API for creating/managing GPU textures */
#include <QtGui/rhi/qrhi.h>

/* D3D11 native texture interface (used together with QRhi D3D11 backend) */
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>

/* WH_KEYBOARD_LL / KBDLLHOOKSTRUCT: 拦截系统级 Win 组合键（如 Win+R）并转发给 RDP */
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#include <cstring>

/* Forward declaration — implemented in mini-qf-client.cc. */
extern void rdp_notify_mouse_moved(double qx, double qy);

#include "freerdp/freerdp.h"
#include "freerdp/client/cliprdr.h"
#include "freerdp/input.h"

#include "qf_util.h"
#include "qf_log.h"

/* Forward declarations from mini-qf-client.cc */
void start_rdp_connection();
void notify_window_resized();

/* =========================================================================
 * RdpFrameTexture — QSGTexture backed by a native D3D11 texture
 *
 * Architecture (D3D11 native texture pipeline):
 *
 *   beforeRendering (render thread):
 *     ensureTexture() — once per resize:
 *       1. Get ID3D11Device / ID3D11DeviceContext from QRhi native handles
 *       2. CreateTexture2D(BIND_SHADER_RESOURCE, DEFAULT, BGRA8)
 *       3. QRhiTexture::createFrom(NativeTexture) to wrap for SG
 *
 *     uploadFrame() — every frame:
 *       4. ID3D11DeviceContext::UpdateSubresource() — zero-copy upload
 *
 *   scene graph render phase:
 *     commitTextureOperations() — no-op (already uploaded in step 4)
 *     rhiTexture() returns the QRhiTexture from step 3
 *
 * Threading:
 *   - ensureTexture() / uploadFrame() : render thread (beforeRendering)
 *   - rhiTexture() / commitTextureOps(): render thread
 * ========================================================================= */
class RdpFrameTexture : public QSGTexture
{
public:
    RdpFrameTexture(QSize size, std::vector<uint8_t>* fb, QMutex* mtx,
                    std::atomic<bool>* dirty)
        : m_size(size)
        , m_frameBuffer(fb)
        , m_mutex(mtx)
        , m_frameDirty(dirty)
    {}

    /* Default constructor — for use as member variable. */
    RdpFrameTexture() = default;

    ~RdpFrameTexture() override { release(); }

    /* -----------------------------------------------------------------
     * D3D11 native texture creation (render thread, beforeRendering).
     *
     * 1. Get ID3D11Device + ID3D11DeviceContext from QRhi
     * 2. CreateTexture2D(BIND_SHADER_RESOURCE | DEFAULT | BGRA8)
     * 3. QRhiTexture::createFrom(NativeTexture) to bridge with SG
     * ----------------------------------------------------------------- */
    void ensureTexture(QQuickWindow* window)
    {
        if (m_rhiTex)
            return;

        if (!window)
            return;

        auto* ri = window->rendererInterface();
        if (!ri) return;

        auto* rhi = static_cast<QRhi*>(
            ri->getResource(window, QSGRendererInterface::RhiResource));
        if (!rhi) return;

        /* Step 1: Get D3D11 device + context from QRhi */
        auto* handles = static_cast<QRhiD3D11NativeHandles*>(
            const_cast<QRhiNativeHandles*>(rhi->nativeHandles()));
        if (!handles || !handles->dev || !handles->context)
            return;

        m_d3dDevice  = static_cast<ID3D11Device*>(handles->dev);
        m_d3dContext = static_cast<ID3D11DeviceContext*>(handles->context);
        m_d3dDevice->AddRef();

        /* Step 2: Create native D3D11 texture */
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width          = static_cast<UINT>(m_size.width());
        desc.Height         = static_cast<UINT>(m_size.height());
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags      = 0;

        if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_nativeTex))
            || !m_nativeTex)
        {
            releaseD3D();
            return;
        }

        /* Step 3: Create QRhiTexture and wrap with createFrom */
        m_rhiTex = rhi->newTexture(QRhiTexture::BGRA8, m_size,
                                   1, QRhiTexture::Flags{});
        if (!m_rhiTex)
        {
            releaseD3D();
            return;
        }

        QRhiTexture::NativeTexture nt{};
        nt.object = reinterpret_cast<quint64>(m_nativeTex);
        nt.layout = 0;

        if (!m_rhiTex->createFrom(nt))
        {
            delete m_rhiTex;
            m_rhiTex = nullptr;
            releaseD3D();
            return;
        }

        qf::log::info("qf.rdp.tex",
                      "D3D11 native texture created ({}x{})",
                      m_size.width(), m_size.height());
    }

    /* -----------------------------------------------------------------
     * uploadFrame — upload CPU frame data to D3D11 native texture via
     *               ID3D11DeviceContext::UpdateSubresource.
     *
     * Called from beforeRendering (render thread), AFTER ensureTexture.
     * ----------------------------------------------------------------- */
    void uploadFrame()
    {
        if (!m_nativeTex || !m_d3dContext)
            return;

        if (!m_frameBuffer || !m_frameDirty)
            return;

        if (!m_frameDirty->load(std::memory_order_acquire))
            return;

        QMutexLocker lock(m_mutex ? m_mutex : nullptr);
        if (m_frameBuffer->empty())
            return;

        const UINT rowPitch = static_cast<UINT>(m_size.width()) * 4;

        m_d3dContext->UpdateSubresource(
            m_nativeTex,
            0,
            nullptr,
            m_frameBuffer->data(),
            rowPitch,
            0);

        m_frameDirty->store(false, std::memory_order_release);
    }

    /* -----------------------------------------------------------------
     * commitTextureOperations — no-op.
     *
     * Data is already uploaded via UpdateSubresource in uploadFrame()
     * during beforeRendering.  No QRhiResourceUpdateBatch needed.
     * ----------------------------------------------------------------- */
    void commitTextureOperations(QRhi* /*rhi*/,
                                 QRhiResourceUpdateBatch* /*resourceUpdates*/) override
    {
        /* NOP — upload already done in uploadFrame() */
    }

    QRhiTexture* rhiTexture() const override { return m_rhiTex; }

    qint64 comparisonKey() const override
    {
        return reinterpret_cast<qint64>(m_rhiTex);
    }

    QSize textureSize() const override { return m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    QSize size() const { return m_size; }
    void setSize(QSize sz) { m_size = sz; }

    void setFrameData(std::vector<uint8_t>* fb, QMutex* mtx,
                      std::atomic<bool>* dirty)
    {
        m_frameBuffer = fb;
        m_mutex = mtx;
        m_frameDirty = dirty;
    }

    /* Release all GPU resources. Safe to call multiple times. */
    void release()
    {
        if (m_rhiTex)
        {
            delete m_rhiTex;
            m_rhiTex = nullptr;
        }
        releaseD3D();
    }

private:
    /* Release D3D11 objects only (without deleting QRhiTexture). */
    void releaseD3D()
    {
        if (m_nativeTex)
        {
            m_nativeTex->Release();
            m_nativeTex = nullptr;
        }
        if (m_d3dDevice)
        {
            m_d3dDevice->Release();
            m_d3dDevice = nullptr;
        }
        m_d3dContext = nullptr;
    }

    QSize                   m_size;
    QRhiTexture*            m_rhiTex = nullptr;

    /* D3D11 native objects */
    ID3D11Texture2D*        m_nativeTex   = nullptr;
    ID3D11Device*           m_d3dDevice   = nullptr;   // AddRef'd by us
    ID3D11DeviceContext*    m_d3dContext   = nullptr;   // borrowed from QRhi

    std::vector<uint8_t>*   m_frameBuffer = nullptr;
    QMutex*                 m_mutex       = nullptr;
    std::atomic<bool>*      m_frameDirty  = nullptr;
};

/* =========================================================================
 * RdpViewItem — QQuickItem-backed RDP view using QSGSimpleTextureNode
 *
 * Rendering pipeline (D3D11 native texture via QRhi):
 *
 *   FreeRDP thread → decodes H.264 frames into gdi->primary_buffer (heap)
 *
 *   FreeRDP thread (EndPaint):
 *     copyFrameData() → copies dirty rect from GDI buffer to CPU staging buffer
 *
 *   Qt main thread (QueuedConnection from EndPaint):
 *     updateGdiFrame() → triggers QQuickItem::update()
 *
 *   Qt scene graph sync phase (GUI thread):
 *     updatePaintNode() → sets RdpFrameTexture on QSGSimpleTextureNode
 *
 *   Qt beforeRendering signal (render thread):
 *     ensureTexture() → CreateTexture2D + createFrom (once per resize)
 *     uploadFrame()   → ID3D11DeviceContext::UpdateSubresource (every frame)
 *
 *   Qt render phase (render thread):
 *     SG renders using the QRhiTexture's D3D11 SRV — no per-frame alloc
 * ========================================================================= */
class RdpViewItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int rdpWidth READ rdpWidth NOTIFY rdpGeometryChanged)
    Q_PROPERTY(int rdpHeight READ rdpHeight NOTIFY rdpGeometryChanged)
    Q_PROPERTY(bool fullscreen READ isFullscreen WRITE setFullscreen NOTIFY fullscreenChanged)

public:
    RdpViewItem(QQuickItem* parent = nullptr) : QQuickItem(parent)
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(QQuickItem::ItemIsFocusScope, true);
        setFocus(true);
        setFlag(QQuickItem::ItemHasContents, true);

        /* Wire up RdpFrameTexture to our frame buffer/mutex/dirty */
        m_rdpTex.setFrameData(&m_frameBuffer, &m_frameMutex, &m_frameDirty);

        /* Install global event filter to intercept ShortcutOverride events
         * when fullscreen, preventing Qt from consuming keys before they
         * reach the RDP session (e.g. Tab, Ctrl+C, direction keys). */
        if (auto* app = QGuiApplication::instance())
            app->installEventFilter(this);

        /* 安装 WH_KEYBOARD_LL 低级键盘钩子拦截 Win 组合键：仅在客户端窗口
         * 处于前台时启用，离开前台（Alt-Tab）后停用，不影响其他应用。 */
        if (auto* app = qApp)
        {
            QObject::connect(app, &QGuiApplication::focusWindowChanged,
                             this, &RdpViewItem::onFocusWindowChanged);
            /* 窗口可能在 connect 前就已获得焦点（focusWindowChanged 不会补发），
             * 主动检查一次当前焦点窗口。 */
            onFocusWindowChanged(app->focusWindow());
        }

        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
                this, &RdpViewItem::dataChangedCallback);

        qf::log::info("view/init", "RdpViewItem created");
    }

    bool isFullscreen() const { return m_fullscreen; }

    void setFullscreen(bool fs)
    {
        if (m_fullscreen != fs)
        {
            m_fullscreen = fs;
            emit fullscreenChanged();
        }
    }

    /* Intercept ShortcutOverride when fullscreen to prevent Qt
     * from consuming shortcut keys before they reach RDP. */
    bool eventFilter(QObject* /*obj*/, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride && m_fullscreen)
        {
            if (QGuiApplication::modalWindow())
                return false;
            event->accept();
            return true;
        }
        return false;
    }

    /* =====================================================================
     * Win 组合键拦截（WH_KEYBOARD_LL 低级键盘钩子）
     *
     * 背景：Win+字母组合是客户端 shell 的系统级快捷键。RegisterHotKey 对
     * Win 组合键被系统保留、注册必然失败，且 Qt 的 ShortcutOverride 也拦
     * 不住。唯一可靠做法：安装 WH_KEYBOARD_LL 低级键盘钩子，它在系统把
     * 按键路由给任何窗口/shell 之前执行——返回非零即可在本地吞掉按键，
     * 同时由我们把按键序列主动转发给 RDP 会话。
     *
     * 策略（仅客户端窗口处于前台时生效）：
     *   - Win 键按下/弹起：本地吞掉（本地开始菜单不再弹出）；
     *   - Win 按住时按下登记的组合键（isInterceptedWinCombo）：吞掉该键，
     *     转发 Win+键 到 RDP（VM 内执行对应系统功能）；
     *   - 单独按 Win（未接组合键）：转发裸 Win（VM 内打开开始菜单）；
     *   - Alt+Tab / Alt+Shift+Tab：吞掉 Tab，转发到 RDP（VM 内切换窗口）；
     *   - Ctrl+Alt+Del 不在此处理（工具栏 sendCtrlAltDelete() 单独发送）。
     * ===================================================================== */

    static inline RdpViewItem* s_hookInstance = nullptr;

    /* 低级键盘钩子回调：由安装钩子的线程（GUI 主线程）调用 */
    static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION && s_hookInstance)
            return s_hookInstance->handleLowLevelKey(wParam, lParam);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    /* 窗口处于前台（激活）时启用钩子，离开前台后停用（按键透传给系统） */
    void onFocusWindowChanged(QWindow* window)
    {
        if (window)
        {
            enableKeyboardHook();
            syncKeyboardToggleState(); /* 回到窗口时重新对齐远端开关状态 */
        }
        else
            disableKeyboardHook();
    }

    /* 同步本机键盘开关状态（NumLock/CapsLock/ScrollLock）到 RDP 会话。
     * 远端默认 NumLock=OFF，若不同步，小键盘数字键（0x47-0x53，与编辑键
     * 共用扫描码）会被远端按编辑键解释（如 1→End），数字输入就会失灵。 */
    void syncKeyboardToggleState()
    {
        if (!m_rdpContext || !m_rdpContext->input)
            return;
        UINT32 syncFlags = 0;
        if (GetKeyState(VK_NUMLOCK) & 1)
            syncFlags |= KBD_SYNC_NUM_LOCK;
        if (GetKeyState(VK_CAPITAL) & 1)
            syncFlags |= KBD_SYNC_CAPS_LOCK;
        if (GetKeyState(VK_SCROLL) & 1)
            syncFlags |= KBD_SYNC_SCROLL_LOCK;
        if (GetKeyState(VK_KANA) & 1)
            syncFlags |= KBD_SYNC_KANA_LOCK;
        freerdp_input_send_synchronize_event(m_rdpContext->input, syncFlags);
        qf::log::info("keyboard/sync", "toggle state synced flags=0x{:08X}", syncFlags);
    }

    void enableKeyboardHook()
    {
        if (m_kbHook)
            return;
        s_hookInstance = this;
        m_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc,
                                     GetModuleHandleW(nullptr), 0);
        if (m_kbHook)
            qf::log::info("hotkey/win", "WH_KEYBOARD_LL hook installed");
        else
            qf::log::warn("hotkey/win", "SetWindowsHookEx failed, error={}",
                          static_cast<int>(GetLastError()));
    }

    void disableKeyboardHook()
    {
        if (m_kbHook)
        {
            UnhookWindowsHookEx(m_kbHook);
            m_kbHook = nullptr;
            m_winDown = false;
            m_comboDown = false;
            m_comboFired = false;
            m_comboScanCode = 0;
            m_altDown = false;
            m_altActive = false;
            m_shiftForwarded = false;
            m_shiftHeld = false;
            m_shiftOwned = false;
            m_snapshotAlt = false;
            m_ctrlDown = false;
            m_caeHandled = false;
            qf::log::info("hotkey/win", "WH_KEYBOARD_LL hook removed");
        }
        if (s_hookInstance == this)
            s_hookInstance = nullptr;
    }

    LRESULT handleLowLevelKey(WPARAM wParam, LPARAM lParam)
    {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (!kb)
            return CallNextHookEx(nullptr, HC_ACTION, wParam, lParam);

        /* 注入的按键（如其它工具 SendInput 产生的）不拦截 */
        if (kb->flags & LLKHF_INJECTED)
            return CallNextHookEx(nullptr, HC_ACTION, wParam, lParam);

        const bool isWinKey = (kb->vkCode == VK_LWIN || kb->vkCode == VK_RWIN);
        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        if (isWinKey)
        {
            if (isDown)
            {
                if (!m_winDown)
                {
                    m_winDown = true;
                    m_comboDown = false; /* 新一轮 Win 会话 */
                    m_comboFired = false;
                }
                return 1; /* 本地吞掉，开始菜单不弹出 */
            }
            if (isUp)
            {
                m_winDown = false;
                if (!m_comboFired)
                {
                    /* 本轮未触发任何组合键：转发裸 Win，打开远端开始菜单 */
                    forwardRdpKey(true, RDP_SCANCODE_LWIN);
                    forwardRdpKey(false, RDP_SCANCODE_LWIN);
                }
                m_comboFired = false;
                return 1;
            }
        }

        /* Win 按住时按下/弹起登记的组合键：吞掉并转发给 RDP（本地 shell 不执行） */
        if (m_winDown && isInterceptedWinCombo(kb->vkCode))
        {
            const UINT scancode = winComboScanCode(kb->vkCode);
            if (isDown)
            {
                if (!m_comboDown)
                {
                    m_comboDown = true;
                    m_comboFired = true; /* 本轮已消费组合键，Win 弹起时不再补裸 Win */
                    m_comboScanCode = scancode;
                    forwardRdpKey(true, RDP_SCANCODE_LWIN);
                    if (m_shiftHeld && !m_shiftOwned) /* Win+Shift+组合（如 Win+Shift+S 截图） */
                    {
                        m_shiftOwned = true;
                        forwardRdpKey(true, RDP_SCANCODE_LSHIFT);
                    }
                    forwardRdpKey(true, scancode);
                }
                return 1;
            }
            if (isUp)
            {
                if (m_comboDown && m_comboScanCode == scancode)
                {
                    m_comboDown = false;
                    forwardRdpKey(false, scancode);
                    if (m_shiftOwned && !m_shiftHeld)
                    {
                        m_shiftOwned = false;
                        forwardRdpKey(false, RDP_SCANCODE_LSHIFT);
                    }
                    forwardRdpKey(false, RDP_SCANCODE_LWIN);
                }
                return 1;
            }
        }

        /* PrintScreen / Alt+PrintScreen：本地吞掉并转发给 RDP（VM 内截图）。
         * 与 mstsc 行为一致；Win 按住时走上面的 Win+PrintScreen 组合分支。 */
        if (kb->vkCode == VK_SNAPSHOT && !m_winDown)
        {
            if (isDown)
            {
                const bool altHeld = m_altDown || (kb->flags & LLKHF_ALTDOWN);
                if (altHeld && !m_snapshotAlt)
                {
                    m_snapshotAlt = true;
                    forwardRdpKey(true, RDP_SCANCODE_LMENU);
                }
                forwardRdpKey(true, RDP_SCANCODE_PRINTSCREEN);
            }
            else
            {
                forwardRdpKey(false, RDP_SCANCODE_PRINTSCREEN);
                if (!m_altDown && m_snapshotAlt) /* 物理 Alt 已松开时补发远端 Alt 弹起 */
                {
                    m_snapshotAlt = false;
                    forwardRdpKey(false, RDP_SCANCODE_LMENU);
                }
            }
            return 1;
        }

        /* Ctrl+Alt+Enter：本地吞掉并切换本地全屏/窗口模式（mstsc 风格），不转发 VM。
         * 仅在全新按下时触发一次，避免按住时重复切换。 */
        if (kb->vkCode == VK_RETURN && m_ctrlDown && m_altDown)
        {
            if (isDown && !m_caeHandled)
            {
                m_caeHandled = true;
                emit toggleFullscreenRequested();
            }
            return 1; /* 本地吞掉，Ctrl+Alt+Enter 不进入远端 */
        }

        /* Alt+Tab / Alt+Shift+Tab：本地吞掉 Tab，转发到 RDP（VM 内切换窗口）。
         * 仅吞 Tab 即可阻止本地任务切换器弹出（Alt 仍透传给本地，不影响 Alt+F4 等）。
         * 按住 Alt 期间远端 Alt 保持按住：首次 Tab 打开切换器，后续每次 Tab 移动
         * 选择，直到物理 Alt 松开才确认并关闭切换器，支持自由选择窗口。 */
        if (kb->vkCode == VK_TAB && (m_altDown || (kb->flags & LLKHF_ALTDOWN)))
        {
            if (isDown)
            {
                if (!m_altActive)
                {
                    m_altActive = true;
                    m_shiftForwarded = false;
                    forwardRdpKey(true, RDP_SCANCODE_LMENU);
                    if (m_shiftHeld)
                    {
                        m_shiftForwarded = true;
                        forwardRdpKey(true, RDP_SCANCODE_LSHIFT);
                    }
                }
                forwardRdpKey(true, RDP_SCANCODE_TAB); /* 每按一次 Tab 移动一次选择 */
                return 1; /* 本地吞掉 Tab，本地任务切换器不会弹出 */
            }
            if (isUp)
            {
                /* 保持 Alt 按住，切换器不关闭 */
                forwardRdpKey(false, RDP_SCANCODE_TAB);
                return 1;
            }
        }

        /* Alt / Shift 键状态跟踪（识别 Alt+Tab 与反向切换） */
        if (kb->vkCode == VK_MENU || kb->vkCode == VK_LMENU || kb->vkCode == VK_RMENU)
        {
            if (isDown)
            {
                m_altDown = true;
            }
            else
            {
                m_altDown = false;
                m_caeHandled = false; /* 组合松开，允许下次 Ctrl+Alt+Enter */
                if (m_altActive)
                {
                    /* 松开 Alt：补发 Shift/Alt 弹起，确认选择并关闭远端切换器 */
                    m_altActive = false;
                    if (m_shiftForwarded)
                    {
                        m_shiftForwarded = false;
                        forwardRdpKey(false, RDP_SCANCODE_LSHIFT);
                    }
                    forwardRdpKey(false, RDP_SCANCODE_LMENU);
                }
                else if (m_snapshotAlt) /* Alt+PrintScreen 后补发远端 Alt 弹起 */
                {
                    m_snapshotAlt = false;
                    forwardRdpKey(false, RDP_SCANCODE_LMENU);
                }
            }
        }
        else if (kb->vkCode == VK_SHIFT || kb->vkCode == VK_LSHIFT || kb->vkCode == VK_RSHIFT)
        {
            m_shiftHeld = isDown;
            if (m_altActive)
            {
                if (isDown && !m_shiftForwarded)
                {
                    m_shiftForwarded = true;
                    forwardRdpKey(true, RDP_SCANCODE_LSHIFT);
                }
                else if (!isDown && m_shiftForwarded)
                {
                    m_shiftForwarded = false;
                    forwardRdpKey(false, RDP_SCANCODE_LSHIFT);
                }
            }
            else if (!isDown && m_shiftOwned) /* Win+Shift+组合 后物理 Shift 弹起：补发远端弹起 */
            {
                m_shiftOwned = false;
                forwardRdpKey(false, RDP_SCANCODE_LSHIFT);
            }
        }
        else if (kb->vkCode == VK_CONTROL || kb->vkCode == VK_LCONTROL || kb->vkCode == VK_RCONTROL)
        {
            m_ctrlDown = isDown;
            if (!isDown)
                m_caeHandled = false; /* Ctrl 松开后允许下次 Ctrl+Alt+Enter */
        }

        return CallNextHookEx(nullptr, HC_ACTION, wParam, lParam);
    }

    /* 需要转发给 RDP 的 Win+组合键登记表（本地 shell 的常见系统快捷键）。
     * 注：Win+Shift+S 由 'S' + Shift 状态处理（不单独登记）。 */
    static bool isInterceptedWinCombo(UINT vkCode)
    {
        switch (vkCode)
        {
            case 'R': /* 运行 */
            case 'E': /* 资源管理器 */
            case 'D': /* 显示桌面 */
            case 'I': /* 设置 */
            case 'M': /* 最小化所有窗口 */
            case 'X': /* 快速链接菜单 */
            case 'S': /* 搜索（含 Win+Shift+S 截图） */
            case 'Q': /* 搜索（Win11 与 Win+S 等价） */
            case 'V': /* 剪贴板历史 */
            case 'G': /* 游戏栏/录屏 */
            case 'P': /* 投影 */
            case 'A': /* 快速设置 */
            case 'W': /* 小组件 */
            case 'T': /* 任务栏循环 */
            case 'B': /* 聚焦通知区域 */
            case 'U': /* 辅助功能设置 */
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': /* Win+数字：启动/切换任务栏第 N 个应用 */
            case VK_HOME:       /* 最小化除当前窗口外所有 */
            case VK_SNAPSHOT:   /* 截图并保存（Win+PrintScreen） */
            case VK_OEM_PERIOD: /* 表情面板（Win+.） */
                return true;
            default:
                return false;
        }
    }

    /* VK 码 → RDP 扫描码。注意：PS/2 扫描码并非按字母顺序线性排列
     * （如 V 的扫描码 0x2F 恰在 R 附近），必须用每键的准确常量。 */
    static constexpr UINT kScancodesAtoZ[26] = {
        RDP_SCANCODE_KEY_A, RDP_SCANCODE_KEY_B, RDP_SCANCODE_KEY_C, RDP_SCANCODE_KEY_D,
        RDP_SCANCODE_KEY_E, RDP_SCANCODE_KEY_F, RDP_SCANCODE_KEY_G, RDP_SCANCODE_KEY_H,
        RDP_SCANCODE_KEY_I, RDP_SCANCODE_KEY_J, RDP_SCANCODE_KEY_K, RDP_SCANCODE_KEY_L,
        RDP_SCANCODE_KEY_M, RDP_SCANCODE_KEY_N, RDP_SCANCODE_KEY_O, RDP_SCANCODE_KEY_P,
        RDP_SCANCODE_KEY_Q, RDP_SCANCODE_KEY_R, RDP_SCANCODE_KEY_S, RDP_SCANCODE_KEY_T,
        RDP_SCANCODE_KEY_U, RDP_SCANCODE_KEY_V, RDP_SCANCODE_KEY_W, RDP_SCANCODE_KEY_X,
        RDP_SCANCODE_KEY_Y, RDP_SCANCODE_KEY_Z,
    };

    static UINT winComboScanCode(UINT vkCode)
    {
        if (vkCode >= 'A' && vkCode <= 'Z')
            return kScancodesAtoZ[vkCode - 'A'];
        /* 主键盘数字行：1-9 连续（0x02-0x0A），0 在末尾（0x0B） */
        if (vkCode >= '1' && vkCode <= '9')
            return static_cast<UINT>(RDP_SCANCODE_KEY_1 + (vkCode - '1'));
        if (vkCode == '0')
            return RDP_SCANCODE_KEY_0;
        switch (vkCode)
        {
            case VK_HOME:        return RDP_SCANCODE_HOME;
            case VK_SNAPSHOT:    return RDP_SCANCODE_PRINTSCREEN;
            case VK_OEM_PERIOD:  return RDP_SCANCODE_OEM_PERIOD;
            default:             return 0;
        }
    }

    /* 向 RDP 会话发送单个按键事件 */
    void forwardRdpKey(bool down, UINT scancode)
    {
        if (!m_rdpContext || !m_rdpContext->input)
            return;
        freerdp_input_send_keyboard_event_ex(m_rdpContext->input,
                                             down ? TRUE : FALSE, FALSE, scancode);
    }

    /* DEBUG: track geometry changes */
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (newGeometry.size() != oldGeometry.size()) {
            qf::log::info("view/geometry", "size {}x{} -> {}x{}",
                          static_cast<int>(oldGeometry.width()),
                          static_cast<int>(oldGeometry.height()),
                          static_cast<int>(newGeometry.width()),
                          static_cast<int>(newGeometry.height()));
        }
    }

    ~RdpViewItem() override
    {
        disableKeyboardHook();
    }

    /* Expose current RDP resolution to QML for poll logging. */
    int rdpWidth() const { return m_rdpWidth; }
    int rdpHeight() const { return m_rdpHeight; }
    void setRdpGeometry(int w, int h)
    {
        if (m_rdpWidth != w || m_rdpHeight != h)
        {
            m_rdpWidth = w;
            m_rdpHeight = h;
            emit rdpGeometryChanged();
        }
    }

    /* Called from QML after layout completes. */
    Q_INVOKABLE void startConnection()
    {
        start_rdp_connection();
    }

    Q_INVOKABLE void notifyWindowResized()
    {
        notify_window_resized();
    }

    /* Called from FreeRDP thread when the connection drops. */
    Q_INVOKABLE void notifyDisconnected()
    {
        m_rdpContext = nullptr;
        m_qfClientContext.reset();
        QCoreApplication::quit();
    }

    void setFreeRDP_context(rdpContext* context)
    {
        m_rdpContext = context;
    }

    void set_qfclient_context(std::shared_ptr<qf::client_t> context)
    {
        m_qfClientContext = std::move(context);
    }

    /* =====================================================================
     * Frame buffer management — CPU staging buffer
     * ===================================================================== */

    /* Called from RDP thread (noop_desktop_resize / my_post_connect).
     * Synchronously updates staging buffer dimensions. */
    void resizeStagingBuffer(uint32_t w, uint32_t h)
    {
        if (w == m_frameWidth && h == m_frameHeight && !m_frameBuffer.empty())
            return;

        QMutexLocker lock(&m_frameMutex);
        m_frameWidth = w;
        m_frameHeight = h;
        m_frameBuffer.resize(static_cast<size_t>(w) * h * 4); // BGRA, 4 Bpp

        const size_t size = static_cast<size_t>(w) * h * 4;
        for (size_t i = 3; i < size; i += 4)
            m_frameBuffer[i] = 0xFF;

        qf::log::info("view/frame", "staging buffer resized to {}x{}", w, h);
    }

    /* Called from GUI thread — triggers a scene graph redraw. */
    void notifyFrameResized()
    {
        update();
    }

    /* Called from FreeRDP thread — copies dirty rect from GDI buffer to staging buffer.
     * Only touches m_frameBuffer (non-Qt memory), protected by mutex. */
    void copyFrameData(const uint8_t* srcBuffer, uint32_t srcStride,
                       int rx, int ry, int rw, int rh)
    {
        if (!srcBuffer || !m_frameWidth || !m_frameHeight)
            return;

        QMutexLocker lock(&m_frameMutex);
        if (m_frameBuffer.empty())
            return;

        /* Clamp dirty rect to frame dimensions */
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > static_cast<int>(m_frameWidth))  rw = m_frameWidth - rx;
        if (ry + rh > static_cast<int>(m_frameHeight)) rh = m_frameHeight - ry;

        if (rw <= 0 || rh <= 0)
            return;

        const int bpp = 4; // BGRA, 4 bytes per pixel
        uint8_t* dst = m_frameBuffer.data();
        const uint32_t dstStride = m_frameWidth * bpp;

        /* Copy dirty rect row by row */
        for (int y = 0; y < rh; y++)
        {
            memcpy(dst + (static_cast<size_t>(ry) + y) * dstStride + static_cast<size_t>(rx) * bpp,
                   srcBuffer + (static_cast<size_t>(ry) + y) * srcStride + static_cast<size_t>(rx) * bpp,
                   static_cast<size_t>(rw) * bpp);
        }

        m_frameDirty.store(true, std::memory_order_release);
    }

    /* Called from Qt main thread (via QueuedConnection from noop_end_paint).
     * Data was already copied by copyFrameData on the FreeRDP thread. */
    void updateGdiFrame(rdpGdi* /*gdi*/, int /*rx*/, int /*ry*/, int /*rw*/, int /*rh*/)
    {
        update();
    }

    void clearFrame()
    {
        update();
    }

    /* =====================================================================
     * Scene graph rendering (sync phase, GUI thread)
     *
     * RdpFrameTexture provides:
     *   - ensureTexture() — CreateTexture2D + createFrom (in beforeRendering)
     *   - uploadFrame()   — ID3D11Context::UpdateSubresource (in beforeRendering)
     * ===================================================================== */
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             QQuickItem::UpdatePaintNodeData*) override
    {
        auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);

        if (!m_frameWidth || !m_frameHeight || boundingRect().isEmpty())
        {
            qf::log::warn("qf.rdp.paint", "no frame/size, deleting node");
            delete node;
            m_rdpTex.release();
            return nullptr;
        }

        const QSize texSize(static_cast<int>(m_frameWidth),
                            static_cast<int>(m_frameHeight));

        /* Recreate QRhiTexture on resize */
        if (m_rdpTex.size() != texSize)
        {
            m_rdpTex.release();
            m_rdpTex.setSize(texSize);
            m_rdpTex.setFrameData(&m_frameBuffer, &m_frameMutex, &m_frameDirty);
            qf::log::info("qf.rdp.paint", "RdpFrameTexture resized to {}x{}",
                          texSize.width(), texSize.height());
        }

        /* Ensure beforeRendering is connected once */
        if (!m_brConnected)
        {
            if (auto* w = window())
            {
                connect(w, &QQuickWindow::beforeRendering, this,
                        [this, w]() {
                    m_rdpTex.ensureTexture(w);
                    m_rdpTex.uploadFrame();
                }, Qt::DirectConnection);
                m_brConnected = true;
                qf::log::info("qf.rdp.paint", "beforeRendering connected");
            }
        }

        bool haveNewFrame = m_frameDirty.load(std::memory_order_acquire);

        if (!haveNewFrame && !node)
        {
            qf::log::info("qf.rdp.paint", "no frame & no node, returning null");
            return nullptr;
        }

        if (!node)
        {
            node = new QSGSimpleTextureNode();
            node->setFiltering(QSGTexture::Nearest);
            qf::log::info("qf.rdp.paint", "new QSGSimpleTextureNode created");
        }

        /* Set our RdpFrameTexture on the node */
        node->setTexture(&m_rdpTex);
        node->setOwnsTexture(false);
        node->setRect(boundingRect());

        return node;
    }

    /* =====================================================================
     * Mouse / Keyboard / Clipboard
     * ===================================================================== */

    std::string get_mouse_flags_string(UINT16 flags) {
        std::string buffer{};
        if (flags & PTR_FLAGS_MOVE) buffer += "MOVE, ";
        if (flags & PTR_FLAGS_DOWN) buffer += "DOWN, ";
        if (flags & PTR_FLAGS_BUTTON1) buffer += "BUTTON1 (Left), ";
        if (flags & PTR_FLAGS_BUTTON2) buffer += "BUTTON2 (Right), ";
        if (flags & PTR_FLAGS_BUTTON3) buffer += "BUTTON3 (Middle), ";
        if (flags & PTR_FLAGS_WHEEL) buffer += "WHEEL, ";
        if (flags & PTR_FLAGS_HWHEEL) buffer += "HWHEEL, ";
        return buffer;
    }

    void mouseEventScaleSend(uint32_t mouse_x, uint32_t mouse_y, uint16_t freerdp_mouse_event) {
        if(!m_rdpContext) return;
        uint32_t host_w = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopWidth);
        uint32_t host_h = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopHeight);
        uint32_t map_x = mouse_x * host_w / width();
        uint32_t map_y = mouse_y * host_h / height();
        if (!freerdp_input_send_mouse_event(m_rdpContext->input, freerdp_mouse_event, map_x, map_y))
            qf::log::warn("input/mouse", "failed to send mouse event flags={} x={} y={}",
                          freerdp_mouse_event, map_x, map_y);
    }

    void mousePressEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN : PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_BUTTON2;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), PTR_FLAGS_MOVE);
        event->accept();
    }
    void hoverMoveEvent(QHoverEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), PTR_FLAGS_MOVE);
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        int delta = event->angleDelta().y();
        uint16_t flags = PTR_FLAGS_WHEEL;
        if (delta < 0) { flags |= PTR_FLAGS_WHEEL_NEGATIVE; delta = -delta; }
        flags |= static_cast<uint16_t>(delta) & WheelRotationMask;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }

    void keyboardUnicodeEventSend(QKeyEvent* event, bool down) {
        if (!m_rdpContext) return;

        const int qkey = event->key();
        const Qt::KeyboardModifiers mods = event->modifiers();

        /* 忽略本地 Ctrl+Alt+Del：系统安全序列（SAK）应用本就无法捕获，
         * 无需转发；如需发送由工具栏 sendCtrlAltDelete() 完成。 */
        if (qkey == Qt::Key_Delete &&
            (mods & Qt::ControlModifier) && (mods & Qt::AltModifier)) {
            return;
        }

        /* 组合键（任一修饰键 + 非修饰键）统一使用物理扫描码转发，不逐个映射。
         * Qt 在组合键下会把 key 报为"转换后的字符键"（如 Shift+= 报 Key_Plus、
         * Shift+1 报 Key_Exclam、Ctrl+Shift+2 报 Key_At），映射表按物理键的 Qt 键值
         * 编写无法覆盖所有组合；而 nativeScanCode 是物理键的原始 PS/2 扫描码
         * （Windows 下与 RDP 扫描码编码一致），远端结合已收到的修饰键状态即可正确解析。 */
        const bool hasModifier =
            mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool isModifierKey =
            (qkey == Qt::Key_Shift || qkey == Qt::Key_Control ||
             qkey == Qt::Key_Alt || qkey == Qt::Key_Meta);
        if (hasModifier && !isModifierKey) {
            const quint32 native = event->nativeScanCode();
            if (native != 0) {
                freerdp_input_send_keyboard_event_ex(m_rdpContext->input, down,
                                                     down && event->isAutoRepeat(), native);
                return;
            }
        }

        UINT32 freerdp_key_code = qf::to_freerdp_key_code(event);
        if (freerdp_key_code == RDP_SCANCODE_UNKNOWN) {
            uint16_t flags = down ? 0 : KBD_FLAGS_RELEASE;
            if (!event->text().isEmpty())
                freerdp_input_send_unicode_keyboard_event(m_rdpContext->input, flags, event->text().unicode()->unicode());
            return;
        }
        freerdp_input_send_keyboard_event_ex(m_rdpContext->input, down,
                                             down && event->isAutoRepeat(), freerdp_key_code);
    }

    void keyPressEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, true); event->accept(); }
    void keyReleaseEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, false); event->accept(); }

    /* Send Ctrl+Alt+Delete to the RDP server (from toolbar button). */
    Q_INVOKABLE void sendCtrlAltDelete()
    {
        if (!m_rdpContext || !m_rdpContext->input)
            return;
        rdpInput* input = m_rdpContext->input;
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
    }

    static QImage imageFromDib(const QByteArray& dib) {
        if (dib.size() < 40) return {};
        const uchar* bytes = reinterpret_cast<const uchar*>(dib.constData());
        const quint32 headerSize = qFromLittleEndian<quint32>(bytes);
        if (headerSize < 12 || static_cast<qsizetype>(headerSize) > dib.size()) return {};
        quint16 bitCount = 0; quint32 compression = 0; quint32 colorUsed = 0;
        if (headerSize >= 40 && dib.size() >= 40) {
            bitCount = qFromLittleEndian<quint16>(bytes + 14);
            compression = qFromLittleEndian<quint32>(bytes + 16);
            colorUsed = qFromLittleEndian<quint32>(bytes + 32);
        }
        quint32 colorTableBytes = 0;
        if (colorUsed > 0) colorTableBytes = colorUsed * 4;
        else if (bitCount > 0 && bitCount <= 8) colorTableBytes = (1u << bitCount) * 4;
        const quint32 bitfieldsBytes = (headerSize == 40 && compression == 3) ? 12 : 0;
        const quint32 pixelOffset = 14 + headerSize + bitfieldsBytes + colorTableBytes;
        const quint32 fileSize = 14 + static_cast<quint32>(dib.size());
        QByteArray bmp; bmp.reserve(static_cast<qsizetype>(fileSize));
        bmp.append('B'); bmp.append('M');
        auto appendLe16 = [&bmp](quint16 v) { char buf[2]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 2); };
        auto appendLe32 = [&bmp](quint32 v) { char buf[4]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 4); };
        appendLe32(fileSize); appendLe16(0); appendLe16(0); appendLe32(pixelOffset);
        bmp.append(dib);
        return QImage::fromData(bmp, "BMP");
    }

    void updateClipboardFilesFromRemote(const std::vector<QString>& paths) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_) return;
        if (paths.empty()) return;
        QList<QUrl> urls;
        for (const auto& path : paths)
            if (QFileInfo::exists(path)) urls.append(QUrl::fromLocalFile(path));
        if (urls.empty()) return;
        m_clipboardDataFromRemote = true;
        auto* data = new QMimeData(); data->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(data);
        m_clipboardDataFromRemote = false;
    }

    void updateClipboardDataFromRemote(const QByteArray& data, uint32_t formatId, const QString& formatName) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_) return;
        m_clipboardDataFromRemote = true;
        if (formatName == QStringLiteral("PNG")) {
            QImage image = QImage::fromData(data, "PNG");
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if (formatId == CF_DIB || formatId == CF_DIBV5) {
            QImage image = imageFromDib(data);
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if(formatId == CF_UNICODETEXT) {
            qsizetype charCount = data.size() / static_cast<qsizetype>(sizeof(char16_t));
            const char16_t* textData = reinterpret_cast<const char16_t*>(data.constData());
            if (charCount > 0 && textData[charCount - 1] == u'\0') --charCount;
            QGuiApplication::clipboard()->setText(QString::fromUtf16(textData, charCount));
        }
        m_clipboardDataFromRemote = false;
    }

    QString clipboardDisplayName(const QFileInfo& root, const QFileInfo& fileInfo) const {
        QString displayName = root.fileName();
        if (root.absoluteFilePath() != fileInfo.absoluteFilePath()) {
            QDir rootDir(root.absoluteFilePath());
            displayName += "/" + rootDir.relativeFilePath(fileInfo.absoluteFilePath());
        }
        displayName.replace("/", "\\"); return displayName;
    }

    void appendClipboardInfoFile(const QFileInfo& root, const QFileInfo& fileInfo) {
        qf::clipboard_info_file_t c;
        c.display_name_ = clipboardDisplayName(root, fileInfo);
        c.local_path_ = fileInfo.absoluteFilePath(); c.total_ = fileInfo.size(); c.is_directory_ = fileInfo.isDir();
        {
            std::lock_guard<std::mutex> lock(m_qfClientContext->clipboard_info_files_mutex_);
            auto it = std::find_if(m_qfClientContext->clipboard_info_files_.begin(),
                m_qfClientContext->clipboard_info_files_.end(),
                [&](const qf::clipboard_info_file_t& f){ return f.local_path_ == fileInfo.absoluteFilePath(); });
            if (it == m_qfClientContext->clipboard_info_files_.end())
                m_qfClientContext->clipboard_info_files_.push_back(c);
        }
        if (!fileInfo.isDir()) return;
        QDir dir(fileInfo.absoluteFilePath());
        for (const QString& f : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            appendClipboardInfoFile(root, QFileInfo(dir.filePath(f)));
    }

    void dataChangedCallback() {
        if (!m_qfClientContext) return;
        auto& clipboardContext = m_qfClientContext->cliprdr_client_context_;
        if(!clipboardContext) return;
        if (m_clipboardDataFromRemote) return;
        auto RemoteClipboardFormatList = [&,this](uint32_t fid, const char* fname) {
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT f = {};
            f.formatId = fid; f.formatName = const_cast<char*>(fname);
            fl.numFormats = 1; fl.formats = &f;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        };
        QClipboard* cb = QGuiApplication::clipboard();
        const QMimeData* md = cb->mimeData();
        if (md->hasUrls()) {
            QByteArray uriList;
            {
                std::lock_guard<std::mutex> lock(m_qfClientContext->clipboard_info_files_mutex_);
                m_qfClientContext->clipboard_info_files_.clear();
            }
            for (const QUrl& url : md->urls()) {
                if (url.isLocalFile()) {
                    QFileInfo file(url.toLocalFile());
                    if (!file.isDir() && !file.isFile()) continue;
                    appendClipboardInfoFile(file, file);
                    uriList.append(url.toEncoded()); uriList.append('\n');
                }
            }
            if (m_qfClientContext->clipboard_info_files_.empty()) return;
            if (m_qfClientContext->cliprdr_file_context_) {
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, TRUE);
                cliprdr_file_context_update_client_data(m_qfClientContext->cliprdr_file_context_,
                    uriList.constData(), static_cast<size_t>(uriList.size()));
            }
            RemoteClipboardFormatList(qf::CLIPBOARD_FORMAT_FILE, qf::CLIPBOARD_FORMAT_FILE_NAME);
        } else if (md->hasText()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            RemoteClipboardFormatList(CF_UNICODETEXT, nullptr);
        } else if (md->hasImage()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT formats[2] = {};
            formats[0].formatId = qf::CLIPBOARD_FORMAT_PNG; formats[0].formatName = const_cast<char*>("PNG");
            formats[1].formatId = CF_DIB; formats[1].formatName = nullptr;
            fl.numFormats = 2; fl.formats = formats;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        }
    }

signals:
    void rdpGeometryChanged();
    void clipboardDataResponseFromRemote();
    void fullscreenChanged();
    void toggleFullscreenRequested(); /* Ctrl+Alt+Enter 本地全屏切换请求 */

private:
    /* Frame buffer — CPU-side copy of the decoded frame */
    std::vector<uint8_t> m_frameBuffer;
    uint32_t             m_frameWidth  = 0;
    uint32_t             m_frameHeight = 0;
    mutable QMutex       m_frameMutex;

    /* Dirty flag for new frame data */
    std::atomic<bool>    m_frameDirty{false};

    /* QRhi-managed texture + QSGTexture wrapper */
    RdpFrameTexture      m_rdpTex;

    /* beforeRendering connection state */
    bool                 m_brConnected = false;

    int                               m_rdpWidth = 0;
    int                               m_rdpHeight = 0;
    rdpContext*                       m_rdpContext = nullptr;
    std::shared_ptr<qf::client_t>     m_qfClientContext;
    bool                              m_clipboardDataFromRemote = false;
    bool                              m_fullscreen = false;

    /* Win 组合键低级键盘钩子状态 */
    HHOOK m_kbHook = nullptr;
    bool  m_winDown     = false; /* 客户端 Win 键当前是否按住 */
    bool  m_comboDown   = false; /* Win+组合键是否已按下并转发 */
    bool  m_comboFired  = false; /* 本轮 Win 会话是否已消费过组合键 */
    UINT  m_comboScanCode = 0;   /* 已转发的组合键 RDP 扫描码 */
    bool  m_altDown        = false; /* Alt 键当前是否按住 */
    bool  m_altActive      = false; /* 远端 Alt 是否已按下（Alt+Tab 会话进行中） */
    bool  m_shiftForwarded = false; /* 远端 Shift 是否已按下（反向切换用） */
    bool  m_shiftHeld      = false; /* Shift 键当前是否按住（Alt+Shift+Tab 反向切换） */
    bool  m_shiftOwned     = false; /* 远端 Shift 是否已按下（Win+Shift+组合 转发用） */
    bool  m_snapshotAlt    = false; /* 远端 Alt 是否已按下（Alt+PrintScreen 转发用） */
    bool  m_ctrlDown       = false; /* Ctrl 键当前是否按住 */
    bool  m_caeHandled     = false; /* Ctrl+Alt+Enter 是否已触发（防重复） */
};
