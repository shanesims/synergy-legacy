#include "CMSWindowsPrimaryScreen.h"
#include "CMSWindowsScreen.h"
#include "IPrimaryScreenReceiver.h"
#include "CPlatform.h"
#include "XScreen.h"
#include "CLog.h"
#include <cstring>

//
// CMSWindowsPrimaryScreen
//

CMSWindowsPrimaryScreen::CMSWindowsPrimaryScreen(
				IScreenReceiver* receiver,
				IPrimaryScreenReceiver* primaryReceiver) :
	CPrimaryScreen(receiver),
	m_receiver(primaryReceiver),
	m_is95Family(CPlatform::isWindows95Family()),
	m_threadID(0),
	m_window(NULL),
	m_mark(0),
	m_markReceived(0)
{
	assert(m_receiver != NULL);

	// load the hook library
	m_hookLibrary = LoadLibrary("synrgyhk");
	if (m_hookLibrary == NULL) {
		log((CLOG_ERR "failed to load hook library"));
		throw XScreenOpenFailure();
	}
	m_setSides  = (SetSidesFunc)GetProcAddress(m_hookLibrary, "setSides");
	m_setZone   = (SetZoneFunc)GetProcAddress(m_hookLibrary, "setZone");
	m_setRelay  = (SetRelayFunc)GetProcAddress(m_hookLibrary, "setRelay");
	m_install   = (InstallFunc)GetProcAddress(m_hookLibrary, "install");
	m_uninstall = (UninstallFunc)GetProcAddress(m_hookLibrary, "uninstall");
	m_init      = (InitFunc)GetProcAddress(m_hookLibrary, "init");
	m_cleanup   = (CleanupFunc)GetProcAddress(m_hookLibrary, "cleanup");
	if (m_setSides  == NULL ||
		m_setZone   == NULL ||
		m_setRelay  == NULL ||
		m_install   == NULL ||
		m_uninstall == NULL ||
		m_init      == NULL ||
		m_cleanup   == NULL) {
		log((CLOG_ERR "invalid hook library"));
		FreeLibrary(m_hookLibrary);
		throw XScreenOpenFailure();
	}

	// create screen
	m_screen = new CMSWindowsScreen(receiver, this);
}

CMSWindowsPrimaryScreen::~CMSWindowsPrimaryScreen()
{
	assert(m_hookLibrary != NULL);
	assert(m_window      == NULL);

	delete m_screen;
	FreeLibrary(m_hookLibrary);
}

void
CMSWindowsPrimaryScreen::reconfigure(UInt32 activeSides)
{
	m_setSides(activeSides);
}

void
CMSWindowsPrimaryScreen::warpCursor(SInt32 x, SInt32 y)
{
	// warp mouse
	warpCursorNoFlush(x, y);

	// remove all input events before and including warp
	MSG msg;
	while (PeekMessage(&msg, NULL, SYNERGY_MSG_INPUT_FIRST,
								SYNERGY_MSG_INPUT_LAST, PM_REMOVE)) {
		// do nothing
	}

	// save position as last position
	m_x = x;
	m_y = y;
}

KeyModifierMask
CMSWindowsPrimaryScreen::getToggleMask() const
{
	KeyModifierMask mask = 0;
	if ((GetKeyState(VK_CAPITAL) & 0x01) != 0)
		mask |= KeyModifierCapsLock;
	if ((GetKeyState(VK_NUMLOCK) & 0x01) != 0)
		mask |= KeyModifierNumLock;
	if ((GetKeyState(VK_SCROLL) & 0x01) != 0)
		mask |= KeyModifierScrollLock;
	return mask;
}

bool
CMSWindowsPrimaryScreen::isLockedToScreen() const
{
	// virtual key table.  the table defines the virtual keys that are
	// mapped to something (including mouse buttons, OEM and kanji keys
	// but not unassigned or undefined keys).
	static const UInt32 s_mappedKeys[] = {
		0xfbff331e,
		0x03ffffff,
		0x3ffffffe,
		0xffffffff,
		0x000300ff,
		0xfc000000,
		0xf8000001,
		0x7ffffe5f
	};

	// check each key.  note that we cannot use GetKeyboardState() here
	// since it reports the state of keys according to key messages
	// that have been pulled off the queue.  in general, we won't get
	// these key messages because they're not for our window.  if any
	// key (or mouse button) is down then we're locked to the screen.
	if (isActive()) {
		// use shadow keyboard state in m_keys
		for (UInt32 i = 0; i < 256; ++i) {
			if ((m_keys[i] & 0x80) != 0) {
				return true;
			}
		}
	}
	else {
		for (UInt32 i = 0; i < 256 / 32; ++i) {
			for (UInt32 b = 1, j = 0; j < 32; b <<= 1, ++j) {
				if ((s_mappedKeys[i] & b) != 0) {
					if (GetAsyncKeyState(i * 32 + j) < 0) {
						return true;
					}
				}
			}
		}
	}

	// not locked
	return false;
}

IScreen*
CMSWindowsPrimaryScreen::getScreen() const
{
	return m_screen;
}

void
CMSWindowsPrimaryScreen::onScreensaver(bool activated)
{
	m_receiver->onScreensaver(activated);
}

bool
CMSWindowsPrimaryScreen::onPreDispatch(const CEvent* event)
{
	assert(event != NULL);

	// handle event
	const MSG* msg = &event->m_msg;
	switch (msg->message) {
	case SYNERGY_MSG_MARK:
		m_markReceived = msg->wParam;
		return true;

	case SYNERGY_MSG_KEY:
		// ignore message if posted prior to last mark change
		if (!ignore()) {
			KeyModifierMask mask;
			const KeyID key = mapKey(msg->wParam, msg->lParam, &mask);
			if (key != kKeyNone) {
				if ((msg->lParam & 0x80000000) == 0) {
					// key press
					const SInt32 repeat = (SInt32)(msg->lParam & 0xffff);
					if (repeat >= 2) {
						log((CLOG_DEBUG1 "event: key repeat key=%d mask=0x%04x count=%d", key, mask, repeat));
						m_receiver->onKeyRepeat(key, mask, repeat);
					}
					else {
						log((CLOG_DEBUG1 "event: key press key=%d mask=0x%04x", key, mask));
						m_receiver->onKeyDown(key, mask);
					}

					// update key state
					updateKey(msg->wParam, true);
				}
				else {
					// key release
					log((CLOG_DEBUG1 "event: key release key=%d mask=0x%04x", key, mask));
					m_receiver->onKeyUp(key, mask);

					// update key state
					updateKey(msg->wParam, false);
				}
			}
			else {
				log((CLOG_DEBUG2 "event: cannot map key wParam=%d lParam=0x%08x", msg->wParam, msg->lParam));
			}
		}
		return true;

	case SYNERGY_MSG_MOUSE_BUTTON:
		// ignore message if posted prior to last mark change
		if (!ignore()) {
			static const int s_vkButton[] = {
				0,				// kButtonNone
				VK_LBUTTON,		// kButtonLeft, etc.
				VK_MBUTTON,
				VK_RBUTTON
			};

			const ButtonID button = mapButton(msg->wParam);
			switch (msg->wParam) {
			case WM_LBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_RBUTTONDOWN:
				log((CLOG_DEBUG1 "event: button press button=%d", button));
				if (button != kButtonNone) {
					m_receiver->onMouseDown(button);
					m_keys[s_vkButton[button]] |= 0x80;
				}
				break;

			case WM_LBUTTONUP:
			case WM_MBUTTONUP:
			case WM_RBUTTONUP:
				log((CLOG_DEBUG1 "event: button release button=%d", button));
				if (button != kButtonNone) {
					m_receiver->onMouseUp(button);
					m_keys[s_vkButton[button]] &= ~0x80;
				}
				break;
			}
		}
		return true;

	case SYNERGY_MSG_MOUSE_WHEEL:
		// ignore message if posted prior to last mark change
		if (!ignore()) {
			log((CLOG_DEBUG1 "event: button wheel delta=%d %d", msg->wParam, msg->lParam));
			m_receiver->onMouseWheel(msg->wParam);
		}
		return true;

	case SYNERGY_MSG_PRE_WARP:
		{
			// save position to compute delta of next motion
			m_x = static_cast<SInt32>(msg->wParam);
			m_y = static_cast<SInt32>(msg->lParam);

			// we warped the mouse.  discard events until we find the
			// matching post warp event.  see warpCursorNoFlush() for
			// where the events are sent.  we discard the matching
			// post warp event and can be sure we've skipped the warp
			// event.
			MSG msg;
			do {
				GetMessage(&msg, NULL, SYNERGY_MSG_MOUSE_MOVE,
										SYNERGY_MSG_POST_WARP);
			} while (msg.message != SYNERGY_MSG_POST_WARP);

			return true;
		}

	case SYNERGY_MSG_POST_WARP:
		log((CLOG_WARN "unmatched post warp"));
		return true;

	case SYNERGY_MSG_MOUSE_MOVE:
		// ignore message if posted prior to last mark change
		if (!ignore()) {
			// compute motion delta (relative to the last known
			// mouse position)
			SInt32 x = static_cast<SInt32>(msg->wParam) - m_x;
			SInt32 y = static_cast<SInt32>(msg->lParam) - m_y;

			// save position to compute delta of next motion
			m_x = static_cast<SInt32>(msg->wParam);
			m_y = static_cast<SInt32>(msg->lParam);

			if (!isActive()) {
				// motion on primary screen
				m_receiver->onMouseMovePrimary(m_x, m_y);
			}
			else {
				// motion on secondary screen.  warp mouse back to
				// center.
				if (x != 0 || y != 0) {
					// back to center
					warpCursorNoFlush(m_xCenter, m_yCenter);

					// send motion
					m_receiver->onMouseMoveSecondary(x, y);
				}
			}
		}
		return true;
	}

	return false;
}

bool
CMSWindowsPrimaryScreen::onEvent(CEvent* event)
{
	assert(event != NULL);

	const MSG& msg = event->m_msg;
	switch (msg.message) {
	case WM_DISPLAYCHANGE:
		// recompute center pixel of primary screen
		m_screen->getCursorCenter(m_xCenter, m_yCenter);

		// warp mouse to center if active
		if (isActive()) {
			warpCursorToCenter();
		}

		// tell hook about resize if not active
		else {
			SInt32 x, y, w, h;
			m_screen->getShape(x, y, w, h);
			m_setZone(x, y, w, h, getJumpZoneSize());
		}
		return true;
	}

	return false;
}

SInt32
CMSWindowsPrimaryScreen::getJumpZoneSize() const
{
	return 1;
}

void
CMSWindowsPrimaryScreen::postCreateWindow(HWND window)
{
	// save window
	m_window = window;

	// install hooks
	m_install();

	// resize window
	// note -- we use a fullscreen window to grab input.  it should
	// be possible to use a 1x1 window but i've run into problems
	// with losing keyboard input (focus?) in that case.
	// unfortunately, hiding the full screen window (when entering
	// the screen) causes all other windows to redraw.
	SInt32 x, y, w, h;
	m_screen->getShape(x, y, w, h);
	MoveWindow(m_window, x, y, w, h, FALSE);

	if (isActive()) {
		// hide the cursor
		showWindow();
	}
	else {
		// watch jump zones
		m_setRelay(false);

		// all messages prior to now are invalid
		nextMark();
	}
}

void
CMSWindowsPrimaryScreen::preDestroyWindow(HWND)
{
	// hide the window if it's visible
	if (isActive()) {
		hideWindow();
	}

	// uninstall hooks
	m_uninstall();
}

void
CMSWindowsPrimaryScreen::onPreMainLoop()
{
	// must call mainLoop() from same thread as open()
	assert(m_threadID == GetCurrentThreadId());
	assert(m_window   != NULL);
}

void
CMSWindowsPrimaryScreen::onPreOpen()
{
	assert(m_window == NULL);

	// initialize hook library
	m_threadID = GetCurrentThreadId();
	if (m_init(m_threadID) == 0) {
		log((CLOG_ERR "cannot initialize hook library"));
		throw XScreenOpenFailure();
	}
}

void
CMSWindowsPrimaryScreen::onPostOpen()
{
	// get cursor info
	m_screen->getCursorPos(m_x, m_y);
	m_screen->getCursorCenter(m_xCenter, m_yCenter);

	// set jump zones
	SInt32 x, y, w, h;
	m_screen->getShape(x, y, w, h);
	m_setZone(x, y, w, h, getJumpZoneSize());

	// initialize marks
	m_mark         = 0;
	m_markReceived = 0;
	nextMark();
}

void
CMSWindowsPrimaryScreen::onPostClose()
{
	m_cleanup();
	m_threadID = 0;
}

void
CMSWindowsPrimaryScreen::onPreEnter()
{
	assert(m_window != NULL);

	// enable ctrl+alt+del, alt+tab, etc
	if (m_is95Family) {
		DWORD dummy = 0;
		SystemParametersInfo(SPI_SETSCREENSAVERRUNNING, FALSE, &dummy, 0);
	}

	// watch jump zones
	m_setRelay(false);
}

void
CMSWindowsPrimaryScreen::onPostEnter()
{
	// all messages prior to now are invalid
	nextMark();
}

void
CMSWindowsPrimaryScreen::onPreLeave()
{
	assert(m_window != NULL);

	// all messages prior to now are invalid
	nextMark();
}

void
CMSWindowsPrimaryScreen::onPostLeave(bool success)
{
	if (success) {
		// relay all mouse and keyboard events
		m_setRelay(true);

		// disable ctrl+alt+del, alt+tab, etc
		if (m_is95Family) {
			DWORD dummy = 0;
			SystemParametersInfo(SPI_SETSCREENSAVERRUNNING, TRUE, &dummy, 0);
		}
	}
}

void
CMSWindowsPrimaryScreen::createWindow()
{
	// open the desktop and the window
	m_window = m_screen->openDesktop();
	if (m_window == NULL) {
		throw XScreenOpenFailure();
	}

	// note -- we use a fullscreen window to grab input.  it should
	// be possible to use a 1x1 window but i've run into problems
	// with losing keyboard input (focus?) in that case.
	// unfortunately, hiding the full screen window (when entering
	// the scren causes all other windows to redraw).
	SInt32 x, y, w, h;
	m_screen->getShape(x, y, w, h);
	MoveWindow(m_window, x, y, w, h, FALSE);
}

void
CMSWindowsPrimaryScreen::destroyWindow()
{
	// close the desktop and the window
	m_screen->closeDesktop();
	m_window = NULL;
}

bool
CMSWindowsPrimaryScreen::showWindow()
{
	// remember the active window before we leave.  GetActiveWindow()
	// will only return the active window for the thread's queue (i.e.
	// our app) but we need the globally active window.  get that by
	// attaching input to the foreground window's thread then calling
	// GetActiveWindow() and then detaching our input.
	m_lastActiveWindow     = NULL;
	m_lastForegroundWindow = GetForegroundWindow();
	m_lastActiveThread     = GetWindowThreadProcessId(
								m_lastForegroundWindow, NULL);
	if (m_lastActiveThread != 0) {
		DWORD myThread = GetCurrentThreadId();
		if (AttachThreadInput(myThread, m_lastActiveThread, TRUE)) {
			m_lastActiveWindow = GetActiveWindow();
			AttachThreadInput(myThread, m_lastActiveThread, FALSE);
		}
	}

	// show our window
	ShowWindow(m_window, SW_SHOW);

	// get keyboard input and capture mouse
	SetActiveWindow(m_window);
	SetFocus(m_window);
	SetCapture(m_window);

	return true;
}

void
CMSWindowsPrimaryScreen::hideWindow()
{
	// restore the active window and hide our window.  we can only set
	// the active window for another thread if we first attach our input
	// to that thread.
	ReleaseCapture();
	if (m_lastActiveWindow != NULL) {
		DWORD myThread = GetCurrentThreadId();
		if (AttachThreadInput(myThread, m_lastActiveThread, TRUE)) {
			// FIXME -- shouldn't raise window if X-Mouse is enabled
			// but i have no idea how to do that or check if enabled.
			SetActiveWindow(m_lastActiveWindow);
			AttachThreadInput(myThread, m_lastActiveThread, FALSE);
		}
	}
	ShowWindow(m_window, SW_HIDE);
}

void
CMSWindowsPrimaryScreen::warpCursorToCenter()
{
	warpCursor(m_xCenter, m_yCenter);
}

void
CMSWindowsPrimaryScreen::warpCursorNoFlush(SInt32 x, SInt32 y)
{
	// send an event that we can recognize before the mouse warp
	PostThreadMessage(m_threadID, SYNERGY_MSG_PRE_WARP, x, y);

	// warp mouse.  hopefully this inserts a mouse motion event
	// between the previous message and the following message.
	SetCursorPos(x, y);

	// send an event that we can recognize after the mouse warp
	PostThreadMessage(m_threadID, SYNERGY_MSG_POST_WARP, 0, 0);
}

void
CMSWindowsPrimaryScreen::nextMark()
{
	// next mark
	++m_mark;

	// mark point in message queue where the mark was changed
	PostThreadMessage(m_threadID, SYNERGY_MSG_MARK, m_mark, 0);
}

bool
CMSWindowsPrimaryScreen::ignore() const
{
	return (m_mark != m_markReceived);
}

static const KeyID		g_virtualKey[] =
{
	/* 0x00 */ kKeyNone,		// reserved
	/* 0x01 */ kKeyNone,		// VK_LBUTTON
	/* 0x02 */ kKeyNone,		// VK_RBUTTON
	/* 0x03 */ kKeyBreak,		// VK_CANCEL
	/* 0x04 */ kKeyNone,		// VK_MBUTTON
	/* 0x05 */ kKeyNone,		// undefined
	/* 0x06 */ kKeyNone,		// undefined
	/* 0x07 */ kKeyNone,		// undefined
	/* 0x08 */ kKeyBackSpace,	// VK_BACK
	/* 0x09 */ kKeyTab,			// VK_TAB
	/* 0x0a */ kKeyNone,		// undefined
	/* 0x0b */ kKeyNone,		// undefined
	/* 0x0c */ kKeyClear,		// VK_CLEAR
	/* 0x0d */ kKeyReturn,		// VK_RETURN
	/* 0x0e */ kKeyNone,		// undefined
	/* 0x0f */ kKeyNone,		// undefined
	/* 0x10 */ kKeyShift_L,		// VK_SHIFT
	/* 0x11 */ kKeyControl_L,	// VK_CONTROL
	/* 0x12 */ kKeyAlt_L,		// VK_MENU
	/* 0x13 */ kKeyPause,		// VK_PAUSE
	/* 0x14 */ kKeyCapsLock,	// VK_CAPITAL
	/* 0x15 */ kKeyNone,		// VK_KANA			
	/* 0x16 */ kKeyNone,		// VK_HANGUL		
	/* 0x17 */ kKeyNone,		// VK_JUNJA			
	/* 0x18 */ kKeyNone,		// VK_FINAL			
	/* 0x19 */ kKeyNone,		// VK_KANJI			
	/* 0x1a */ kKeyNone,		// undefined
	/* 0x1b */ kKeyEscape,		// VK_ESCAPE
	/* 0x1c */ kKeyNone,		// VK_CONVERT		
	/* 0x1d */ kKeyNone,		// VK_NONCONVERT	
	/* 0x1e */ kKeyNone,		// VK_ACCEPT		
	/* 0x1f */ kKeyNone,		// VK_MODECHANGE	
	/* 0x20 */ 0x0020,			// VK_SPACE
	/* 0x21 */ kKeyPageUp,		// VK_PRIOR
	/* 0x22 */ kKeyPageDown,	// VK_NEXT
	/* 0x23 */ kKeyEnd,			// VK_END
	/* 0x24 */ kKeyHome,		// VK_HOME
	/* 0x25 */ kKeyLeft,		// VK_LEFT
	/* 0x26 */ kKeyUp,			// VK_UP
	/* 0x27 */ kKeyRight,		// VK_RIGHT
	/* 0x28 */ kKeyDown,		// VK_DOWN
	/* 0x29 */ kKeySelect,		// VK_SELECT
	/* 0x2a */ kKeyNone,		// VK_PRINT
	/* 0x2b */ kKeyExecute,		// VK_EXECUTE
	/* 0x2c */ kKeyPrint,		// VK_SNAPSHOT
	/* 0x2d */ kKeyInsert,		// VK_INSERT
	/* 0x2e */ kKeyDelete,		// VK_DELETE
	/* 0x2f */ kKeyHelp,		// VK_HELP
	/* 0x30 */ kKeyNone,		// VK_0
	/* 0x31 */ kKeyNone,		// VK_1
	/* 0x32 */ kKeyNone,		// VK_2
	/* 0x33 */ kKeyNone,		// VK_3
	/* 0x34 */ kKeyNone,		// VK_4
	/* 0x35 */ kKeyNone,		// VK_5
	/* 0x36 */ kKeyNone,		// VK_6
	/* 0x37 */ kKeyNone,		// VK_7
	/* 0x38 */ kKeyNone,		// VK_8
	/* 0x39 */ kKeyNone,		// VK_9
	/* 0x3a */ kKeyNone,		// undefined
	/* 0x3b */ kKeyNone,		// undefined
	/* 0x3c */ kKeyNone,		// undefined
	/* 0x3d */ kKeyNone,		// undefined
	/* 0x3e */ kKeyNone,		// undefined
	/* 0x3f */ kKeyNone,		// undefined
	/* 0x40 */ kKeyNone,		// undefined
	/* 0x41 */ kKeyNone,		// VK_A
	/* 0x42 */ kKeyNone,		// VK_B
	/* 0x43 */ kKeyNone,		// VK_C
	/* 0x44 */ kKeyNone,		// VK_D
	/* 0x45 */ kKeyNone,		// VK_E
	/* 0x46 */ kKeyNone,		// VK_F
	/* 0x47 */ kKeyNone,		// VK_G
	/* 0x48 */ kKeyNone,		// VK_H
	/* 0x49 */ kKeyNone,		// VK_I
	/* 0x4a */ kKeyNone,		// VK_J
	/* 0x4b */ kKeyNone,		// VK_K
	/* 0x4c */ kKeyNone,		// VK_L
	/* 0x4d */ kKeyNone,		// VK_M
	/* 0x4e */ kKeyNone,		// VK_N
	/* 0x4f */ kKeyNone,		// VK_O
	/* 0x50 */ kKeyNone,		// VK_P
	/* 0x51 */ kKeyNone,		// VK_Q
	/* 0x52 */ kKeyNone,		// VK_R
	/* 0x53 */ kKeyNone,		// VK_S
	/* 0x54 */ kKeyNone,		// VK_T
	/* 0x55 */ kKeyNone,		// VK_U
	/* 0x56 */ kKeyNone,		// VK_V
	/* 0x57 */ kKeyNone,		// VK_W
	/* 0x58 */ kKeyNone,		// VK_X
	/* 0x59 */ kKeyNone,		// VK_Y
	/* 0x5a */ kKeyNone,		// VK_Z
	/* 0x5b */ kKeyMeta_L,		// VK_LWIN
	/* 0x5c */ kKeyMeta_R,		// VK_RWIN
	/* 0x5d */ kKeyMenu,		// VK_APPS
	/* 0x5e */ kKeyNone,		// undefined
	/* 0x5f */ kKeyNone,		// undefined
	/* 0x60 */ kKeyKP_0,		// VK_NUMPAD0
	/* 0x61 */ kKeyKP_1,		// VK_NUMPAD1
	/* 0x62 */ kKeyKP_2,		// VK_NUMPAD2
	/* 0x63 */ kKeyKP_3,		// VK_NUMPAD3
	/* 0x64 */ kKeyKP_4,		// VK_NUMPAD4
	/* 0x65 */ kKeyKP_5,		// VK_NUMPAD5
	/* 0x66 */ kKeyKP_6,		// VK_NUMPAD6
	/* 0x67 */ kKeyKP_7,		// VK_NUMPAD7
	/* 0x68 */ kKeyKP_8,		// VK_NUMPAD8
	/* 0x69 */ kKeyKP_9,		// VK_NUMPAD9
	/* 0x6a */ kKeyKP_Multiply,	// VK_MULTIPLY
	/* 0x6b */ kKeyKP_Add,		// VK_ADD
	/* 0x6c */ kKeyKP_Separator,// VK_SEPARATOR
	/* 0x6d */ kKeyKP_Subtract,	// VK_SUBTRACT
	/* 0x6e */ kKeyKP_Decimal,	// VK_DECIMAL
	/* 0x6f */ kKeyKP_Divide,	// VK_DIVIDE
	/* 0x70 */ kKeyF1,			// VK_F1
	/* 0x71 */ kKeyF2,			// VK_F2
	/* 0x72 */ kKeyF3,			// VK_F3
	/* 0x73 */ kKeyF4,			// VK_F4
	/* 0x74 */ kKeyF5,			// VK_F5
	/* 0x75 */ kKeyF6,			// VK_F6
	/* 0x76 */ kKeyF7,			// VK_F7
	/* 0x77 */ kKeyF8,			// VK_F8
	/* 0x78 */ kKeyF9,			// VK_F9
	/* 0x79 */ kKeyF10,			// VK_F10
	/* 0x7a */ kKeyF11,			// VK_F11
	/* 0x7b */ kKeyF12,			// VK_F12
	/* 0x7c */ kKeyF13,			// VK_F13
	/* 0x7d */ kKeyF14,			// VK_F14
	/* 0x7e */ kKeyF15,			// VK_F15
	/* 0x7f */ kKeyF16,			// VK_F16
	/* 0x80 */ kKeyF17,			// VK_F17
	/* 0x81 */ kKeyF18,			// VK_F18
	/* 0x82 */ kKeyF19,			// VK_F19
	/* 0x83 */ kKeyF20,			// VK_F20
	/* 0x84 */ kKeyF21,			// VK_F21
	/* 0x85 */ kKeyF22,			// VK_F22
	/* 0x86 */ kKeyF23,			// VK_F23
	/* 0x87 */ kKeyF24,			// VK_F24
	/* 0x88 */ kKeyNone,		// unassigned
	/* 0x89 */ kKeyNone,		// unassigned
	/* 0x8a */ kKeyNone,		// unassigned
	/* 0x8b */ kKeyNone,		// unassigned
	/* 0x8c */ kKeyNone,		// unassigned
	/* 0x8d */ kKeyNone,		// unassigned
	/* 0x8e */ kKeyNone,		// unassigned
	/* 0x8f */ kKeyNone,		// unassigned
	/* 0x90 */ kKeyNumLock,		// VK_NUMLOCK
	/* 0x91 */ kKeyScrollLock,	// VK_SCROLL
	/* 0x92 */ kKeyNone,		// unassigned
	/* 0x93 */ kKeyNone,		// unassigned
	/* 0x94 */ kKeyNone,		// unassigned
	/* 0x95 */ kKeyNone,		// unassigned
	/* 0x96 */ kKeyNone,		// unassigned
	/* 0x97 */ kKeyNone,		// unassigned
	/* 0x98 */ kKeyNone,		// unassigned
	/* 0x99 */ kKeyNone,		// unassigned
	/* 0x9a */ kKeyNone,		// unassigned
	/* 0x9b */ kKeyNone,		// unassigned
	/* 0x9c */ kKeyNone,		// unassigned
	/* 0x9d */ kKeyNone,		// unassigned
	/* 0x9e */ kKeyNone,		// unassigned
	/* 0x9f */ kKeyNone,		// unassigned
	/* 0xa0 */ kKeyShift_L,		// VK_LSHIFT
	/* 0xa1 */ kKeyShift_R,		// VK_RSHIFT
	/* 0xa2 */ kKeyControl_L,	// VK_LCONTROL
	/* 0xa3 */ kKeyControl_R,	// VK_RCONTROL
	/* 0xa4 */ kKeyAlt_L,		// VK_LMENU
	/* 0xa5 */ kKeyAlt_R,		// VK_RMENU
	/* 0xa6 */ kKeyNone,		// unassigned
	/* 0xa7 */ kKeyNone,		// unassigned
	/* 0xa8 */ kKeyNone,		// unassigned
	/* 0xa9 */ kKeyNone,		// unassigned
	/* 0xaa */ kKeyNone,		// unassigned
	/* 0xab */ kKeyNone,		// unassigned
	/* 0xac */ kKeyNone,		// unassigned
	/* 0xad */ kKeyNone,		// unassigned
	/* 0xae */ kKeyNone,		// unassigned
	/* 0xaf */ kKeyNone,		// unassigned
	/* 0xb0 */ kKeyNone,		// unassigned
	/* 0xb1 */ kKeyNone,		// unassigned
	/* 0xb2 */ kKeyNone,		// unassigned
	/* 0xb3 */ kKeyNone,		// unassigned
	/* 0xb4 */ kKeyNone,		// unassigned
	/* 0xb5 */ kKeyNone,		// unassigned
	/* 0xb6 */ kKeyNone,		// unassigned
	/* 0xb7 */ kKeyNone,		// unassigned
	/* 0xb8 */ kKeyNone,		// unassigned
	/* 0xb9 */ kKeyNone,		// unassigned
	/* 0xba */ kKeyNone,		// OEM specific
	/* 0xbb */ kKeyNone,		// OEM specific
	/* 0xbc */ kKeyNone,		// OEM specific
	/* 0xbd */ kKeyNone,		// OEM specific
	/* 0xbe */ kKeyNone,		// OEM specific
	/* 0xbf */ kKeyNone,		// OEM specific
	/* 0xc0 */ kKeyNone,		// OEM specific
	/* 0xc1 */ kKeyNone,		// unassigned
	/* 0xc2 */ kKeyNone,		// unassigned
	/* 0xc3 */ kKeyNone,		// unassigned
	/* 0xc4 */ kKeyNone,		// unassigned
	/* 0xc5 */ kKeyNone,		// unassigned
	/* 0xc6 */ kKeyNone,		// unassigned
	/* 0xc7 */ kKeyNone,		// unassigned
	/* 0xc8 */ kKeyNone,		// unassigned
	/* 0xc9 */ kKeyNone,		// unassigned
	/* 0xca */ kKeyNone,		// unassigned
	/* 0xcb */ kKeyNone,		// unassigned
	/* 0xcc */ kKeyNone,		// unassigned
	/* 0xcd */ kKeyNone,		// unassigned
	/* 0xce */ kKeyNone,		// unassigned
	/* 0xcf */ kKeyNone,		// unassigned
	/* 0xd0 */ kKeyNone,		// unassigned
	/* 0xd1 */ kKeyNone,		// unassigned
	/* 0xd2 */ kKeyNone,		// unassigned
	/* 0xd3 */ kKeyNone,		// unassigned
	/* 0xd4 */ kKeyNone,		// unassigned
	/* 0xd5 */ kKeyNone,		// unassigned
	/* 0xd6 */ kKeyNone,		// unassigned
	/* 0xd7 */ kKeyNone,		// unassigned
	/* 0xd8 */ kKeyNone,		// unassigned
	/* 0xd9 */ kKeyNone,		// unassigned
	/* 0xda */ kKeyNone,		// unassigned
	/* 0xdb */ kKeyNone,		// OEM specific
	/* 0xdc */ kKeyNone,		// OEM specific
	/* 0xdd */ kKeyNone,		// OEM specific
	/* 0xde */ kKeyNone,		// OEM specific
	/* 0xdf */ kKeyNone,		// OEM specific
	/* 0xe0 */ kKeyNone,		// OEM specific
	/* 0xe1 */ kKeyNone,		// OEM specific
	/* 0xe2 */ kKeyNone,		// OEM specific
	/* 0xe3 */ kKeyNone,		// OEM specific
	/* 0xe4 */ kKeyNone,		// OEM specific
	/* 0xe5 */ kKeyNone,		// unassigned
	/* 0xe6 */ kKeyNone,		// OEM specific
	/* 0xe7 */ kKeyNone,		// unassigned
	/* 0xe8 */ kKeyNone,		// unassigned
	/* 0xe9 */ kKeyNone,		// OEM specific
	/* 0xea */ kKeyNone,		// OEM specific
	/* 0xeb */ kKeyNone,		// OEM specific
	/* 0xec */ kKeyNone,		// OEM specific
	/* 0xed */ kKeyNone,		// OEM specific
	/* 0xee */ kKeyNone,		// OEM specific
	/* 0xef */ kKeyNone,		// OEM specific
	/* 0xf0 */ kKeyNone,		// OEM specific
	/* 0xf1 */ kKeyNone,		// OEM specific
	/* 0xf2 */ kKeyNone,		// OEM specific
	/* 0xf3 */ kKeyNone,		// OEM specific
	/* 0xf4 */ kKeyNone,		// OEM specific
	/* 0xf5 */ kKeyNone,		// OEM specific
	/* 0xf6 */ kKeyNone,		// VK_ATTN			
	/* 0xf7 */ kKeyNone,		// VK_CRSEL			
	/* 0xf8 */ kKeyNone,		// VK_EXSEL			
	/* 0xf9 */ kKeyNone,		// VK_EREOF			
	/* 0xfa */ kKeyNone,		// VK_PLAY			
	/* 0xfb */ kKeyNone,		// VK_ZOOM			
	/* 0xfc */ kKeyNone,		// reserved
	/* 0xfd */ kKeyNone,		// VK_PA1			
	/* 0xfe */ kKeyNone,		// VK_OEM_CLEAR		
	/* 0xff */ kKeyNone			// reserved
};

KeyID
CMSWindowsPrimaryScreen::mapKey(
	WPARAM vkCode,
	LPARAM info,
	KeyModifierMask* maskOut)
{
	// note:  known microsoft bugs
	//  Q72583 -- MapVirtualKey() maps keypad keys incorrectly
	//    95,98: num pad vk code -> invalid scan code
	//    95,98,NT4: num pad scan code -> bad vk code except
	//      SEPARATOR, MULTIPLY, SUBTRACT, ADD

	assert(maskOut != NULL);

	// map modifier key
	KeyModifierMask mask = 0;
	if (((m_keys[VK_LSHIFT] |
		  m_keys[VK_RSHIFT] |
		  m_keys[VK_SHIFT]) & 0x80) != 0) {
		mask |= KeyModifierShift;
	}
	if (((m_keys[VK_LCONTROL] |
		  m_keys[VK_RCONTROL] |
		  m_keys[VK_CONTROL]) & 0x80) != 0) {
		mask |= KeyModifierControl;
	}
	if (((m_keys[VK_LMENU] |
		  m_keys[VK_RMENU] |
		  m_keys[VK_MENU]) & 0x80) != 0) {
		mask |= KeyModifierAlt;
	}
	if (((m_keys[VK_LWIN] |
		  m_keys[VK_RWIN]) & 0x80) != 0) {
		mask |= KeyModifierMeta;
	}
	if ((m_keys[VK_CAPITAL] & 0x01) != 0) {
		mask |= KeyModifierCapsLock;
	}
	if ((m_keys[VK_NUMLOCK] & 0x01) != 0) {
		mask |= KeyModifierNumLock;
	}
	if ((m_keys[VK_SCROLL] & 0x01) != 0) {
		mask |= KeyModifierScrollLock;
	}
	*maskOut = mask;
	log((CLOG_DEBUG2 "key in vk=%d info=0x%08x mask=0x%04x", vkCode, info, mask));

	// get the scan code
	UINT scanCode = static_cast<UINT>((info & 0xff0000) >> 16);

	// convert virtual key to one that distinguishes between left and
	// right for keys that have left/right versions.  known scan codes
	// that don't have left/right versions are passed through unchanged.
	// unknown scan codes return 0.
	UINT vkCode2 = MapVirtualKey(scanCode, 3);

	// work around bug Q72583 (bad num pad conversion in MapVirtualKey())
	if (vkCode >= VK_NUMPAD0 && vkCode <= VK_DIVIDE) {
		vkCode2 = vkCode;
	}

	// MapVirtualKey() appears to map VK_LWIN, VK_RWIN, VK_APPS to
	// some other meaningless virtual key.  work around that bug.
	else if (vkCode >= VK_LWIN && vkCode <= VK_APPS) {
		vkCode2 = vkCode;
	}

	// if MapVirtualKey failed then use original virtual key
	else if (vkCode2 == 0) {
		vkCode2 = vkCode;
	}

	// sadly, win32 will not distinguish between the left and right
	// control and alt keys using the above function.  however, we
	// can check for those:  if bit 24 of info is set then the key
	// is a "extended" key, such as the right control and right alt
	// keys.
	if ((info & 0x1000000) != 0) {
		switch (vkCode2) {
		case VK_CONTROL:
		case VK_LCONTROL:
			vkCode2 = VK_RCONTROL;
			break;

		case VK_MENU:
		case VK_LMENU:
			vkCode2 = VK_RMENU;
			break;
		}
	}

	// use left/right distinguishing virtual key
	vkCode = vkCode2;
	log((CLOG_DEBUG1 "key vk=%d scan=%d", vkCode, scanCode));

	// handle some keys via table lookup
	KeyID id = g_virtualKey[vkCode];
	if (id != kKeyNone) {
		return id;
	}

	// check for dead keys
	if (MapVirtualKey(vkCode, 2) >= 0x8000) {
		return kKeyMultiKey;
	}

	// ToAscii() maps ctrl+letter to the corresponding control code
	// and ctrl+backspace to delete.  if we've got a control code or
	// delete then do ToAscii() again but without the control state.
	// ToAscii() interprets the control modifier state which we don't
	// want.  so save the control state then clear it.
	BYTE lControl       = m_keys[VK_LCONTROL];
	BYTE rControl       = m_keys[VK_RCONTROL];
	BYTE control        = m_keys[VK_CONTROL];
	m_keys[VK_LCONTROL] = 0;
	m_keys[VK_RCONTROL] = 0;
	m_keys[VK_CONTROL]  = 0;

	// convert to ascii
	// FIXME -- support unicode
	WORD ascii;
	int result = ToAscii(vkCode, scanCode, m_keys, &ascii, 0);

	// restore control state
	m_keys[VK_LCONTROL] = lControl;
	m_keys[VK_RCONTROL] = rControl;
	m_keys[VK_CONTROL]  = control;

	// if result is less than zero then it was a dead key.  that key
	// is remembered by the keyboard which we don't want.  remove it
	// by calling ToAscii() again with arbitrary arguments.
	if (result < 0) {
		ToAscii(vkCode, scanCode, m_keys, &ascii, 0);
		return kKeyMultiKey;
	}

	// if result is 1 then the key was succesfully converted
	else if (result == 1) {
		return static_cast<KeyID>(ascii & 0x00ff);
	}

	// if result is 2 then a previous dead key could not be composed.
	// put the old dead key back.
	else if (result == 2) {
		// get the scan code of the dead key and the shift state
		// required to generate it.
		vkCode = VkKeyScan(static_cast<TCHAR>(ascii & 0x00ff));

		// set shift state required to generate key
		BYTE keys[256];
		memset(keys, 0, sizeof(keys));
		if (vkCode & 0x0100) {
			keys[VK_SHIFT]   = 0x80;
		}
		if (vkCode & 0x0200) {
			keys[VK_CONTROL] = 0x80;
		}
		if (vkCode & 0x0400) {
			keys[VK_MENU]    = 0x80;
		}

		// strip shift state off of virtual key code
		vkCode &= 0x00ff;

		// get the scan code for the key
		scanCode = MapVirtualKey(vkCode, 0);

		// put it back
		ToAscii(vkCode, scanCode, keys, &ascii, 0);
		return kKeyMultiKey;
	}

	// cannot convert key
	return kKeyNone;
}

ButtonID
CMSWindowsPrimaryScreen::mapButton(WPARAM button) const
{
	switch (button) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		return kButtonLeft;

	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		return kButtonMiddle;

	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		return kButtonRight;

	default:
		return kButtonNone;
	}
}

void
CMSWindowsPrimaryScreen::updateKeys()
{
	// not using GetKeyboardState() because that doesn't seem to give
	// up-to-date results.  i don't know why that is or why GetKeyState()
	// should give different results.

	// clear key state
	memset(m_keys, 0, sizeof(m_keys));

	// we only care about the modifier key states.  other keys and the
	// mouse buttons should be up.
	m_keys[VK_LSHIFT]   = static_cast<BYTE>(GetKeyState(VK_LSHIFT));
	m_keys[VK_RSHIFT]   = static_cast<BYTE>(GetKeyState(VK_RSHIFT));
	m_keys[VK_SHIFT]    = static_cast<BYTE>(GetKeyState(VK_SHIFT));
	m_keys[VK_LCONTROL] = static_cast<BYTE>(GetKeyState(VK_LCONTROL));
	m_keys[VK_RCONTROL] = static_cast<BYTE>(GetKeyState(VK_RCONTROL));
	m_keys[VK_CONTROL]  = static_cast<BYTE>(GetKeyState(VK_CONTROL));
	m_keys[VK_LMENU]    = static_cast<BYTE>(GetKeyState(VK_LMENU));
	m_keys[VK_RMENU]    = static_cast<BYTE>(GetKeyState(VK_RMENU));
	m_keys[VK_MENU]     = static_cast<BYTE>(GetKeyState(VK_MENU));
	m_keys[VK_LWIN]     = static_cast<BYTE>(GetKeyState(VK_LWIN));
	m_keys[VK_RWIN]     = static_cast<BYTE>(GetKeyState(VK_RWIN));
	m_keys[VK_APPS]     = static_cast<BYTE>(GetKeyState(VK_APPS));
	m_keys[VK_CAPITAL]  = static_cast<BYTE>(GetKeyState(VK_CAPITAL));
	m_keys[VK_NUMLOCK]  = static_cast<BYTE>(GetKeyState(VK_NUMLOCK));
	m_keys[VK_SCROLL]   = static_cast<BYTE>(GetKeyState(VK_SCROLL));
}

void
CMSWindowsPrimaryScreen::updateKey(UINT vkCode, bool press)
{
	if (press) {
		switch (vkCode) {
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_SHIFT:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_SHIFT]   |= 0x80;
			break;

		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_CONTROL:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_CONTROL] |= 0x80;
			break;

		case VK_LMENU:
		case VK_RMENU:
		case VK_MENU:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_MENU]    |= 0x80;
			break;

		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			// toggle keys
			m_keys[vkCode]     |= 0x80;
			break;

		default:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
			m_keys[vkCode]     |= 0x80;
			break;
		}
	}
	else {
		switch (vkCode) {
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_SHIFT:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LSHIFT] | m_keys[VK_RSHIFT]) & 0x80) == 0) {
				m_keys[VK_SHIFT] &= ~0x80;
			}
			break;

		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_CONTROL:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LCONTROL] | m_keys[VK_RCONTROL]) & 0x80) == 0) {
				m_keys[VK_CONTROL] &= ~0x80;
			}
			break;

		case VK_LMENU:
		case VK_RMENU:
		case VK_MENU:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LMENU] | m_keys[VK_RMENU]) & 0x80) == 0) {
				m_keys[VK_MENU] &= ~0x80;
			}
			break;

		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			// toggle keys
			m_keys[vkCode]     &= ~0x80;
			m_keys[vkCode]     ^=  0x01;
			break;

		default:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
			m_keys[vkCode]     &= ~0x80;
			break;
		}
	}
}