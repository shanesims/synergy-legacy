#include "CServer.h"
#include "CHTTPServer.h"
#include "CPrimaryClient.h"
#include "IPrimaryScreenFactory.h"
#include "CInputPacketStream.h"
#include "COutputPacketStream.h"
#include "CProtocolUtil.h"
#include "CClientProxy1_0.h"
#include "ProtocolTypes.h"
#include "XScreen.h"
#include "XSynergy.h"
#include "CTCPListenSocket.h"
#include "IDataSocket.h"
#include "ISocketFactory.h"
#include "XSocket.h"
#include "IStreamFilterFactory.h"
#include "CLock.h"
#include "CThread.h"
#include "CTimerThread.h"
#include "XThread.h"
#include "CFunctionJob.h"
#include "CLog.h"
#include "CStopwatch.h"
#include "TMethodJob.h"

//
// CServer
//

const SInt32			CServer::s_httpMaxSimultaneousRequests = 3;

CServer::CServer(const CString& serverName) :
	m_name(serverName),
	m_bindTimeout(5.0 * 60.0),
	m_screenFactory(NULL),
	m_socketFactory(NULL),
	m_streamFilterFactory(NULL),
	m_acceptClientThread(NULL),
	m_active(NULL),
	m_primaryClient(NULL),
	m_seqNum(0),
	m_activeSaver(NULL),
	m_httpServer(NULL),
	m_httpAvailable(&m_mutex, s_httpMaxSimultaneousRequests)
{
	// do nothing
}

CServer::~CServer()
{
	delete m_screenFactory;
	delete m_socketFactory;
	delete m_streamFilterFactory;
}

void
CServer::open()
{
	// open the screen
	try {
		log((CLOG_INFO "opening screen"));
		openPrimaryScreen();
	}
	catch (XScreen&) {
		// can't open screen
		log((CLOG_INFO "failed to open screen"));
		throw;
	}
	catch (XUnknownClient& e) {
		// can't open screen
		log((CLOG_CRIT "unknown screen name `%s'", e.getName().c_str()));
		throw;
	}
}

void
CServer::mainLoop()
{
	// check preconditions
	{
		CLock lock(&m_mutex);
		assert(m_primaryClient != NULL);
	}

	try {
		log((CLOG_NOTE "starting server"));

		// start listening for new clients
		m_acceptClientThread = new CThread(startThread(
								new TMethodJob<CServer>(this,
									&CServer::acceptClients)));

		// start listening for HTTP requests
		if (m_config.getHTTPAddress().isValid()) {
			m_httpServer = new CHTTPServer(this);
			startThread(new TMethodJob<CServer>(this,
								&CServer::acceptHTTPClients));
		}

		// handle events
		m_primaryClient->mainLoop();

		// clean up
		log((CLOG_NOTE "stopping server"));

		// use a macro to write the stuff that should go into a finally
		// block so we can repeat it easily.  stroustrup's view that
		// "resource acquistion is initialization" is a better solution
		// than a finally block is parochial.  they both have their
		// place.  adding finally to C++ would've been a drop in a big
		// bucket.
#define FINALLY do {					\
		stopThreads();					\
		delete m_httpServer;			\
		m_httpServer = NULL;			\
		} while (false)
		FINALLY;
	}
	catch (XBase& e) {
		log((CLOG_ERR "server error: %s", e.what()));

		// clean up
		log((CLOG_NOTE "stopping server"));
		FINALLY;
	}
	catch (XThread&) {
		// clean up
		log((CLOG_NOTE "stopping server"));
		FINALLY;
		throw;
	}
	catch (...) {
		log((CLOG_DEBUG "unknown server error"));

		// clean up
		log((CLOG_NOTE "stopping server"));
		FINALLY;
		throw;
	}
#undef FINALLY
}

void
CServer::exitMainLoop()
{
	m_primaryClient->exitMainLoop();
}

void
CServer::close()
{
	if (m_primaryClient != NULL) {
		closePrimaryScreen();
	}
	log((CLOG_INFO "closed screen"));
}

bool
CServer::setConfig(const CConfig& config)
{
	// refuse configuration if it doesn't include the primary screen
	{
		CLock lock(&m_mutex);
		if (m_primaryClient != NULL &&
			!config.isScreen(m_primaryClient->getName())) {
			return false;
		}
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	CLock lock(&m_mutex);
	m_config = config;

	// tell primary screen about reconfiguration
	if (m_primaryClient != NULL) {
		m_primaryClient->reconfigure(getActivePrimarySides());
	}

	return true;
}

void
CServer::setScreenFactory(IPrimaryScreenFactory* adopted)
{
	CLock lock(&m_mutex);
	delete m_screenFactory;
	m_screenFactory = adopted;
}

void
CServer::setSocketFactory(ISocketFactory* adopted)
{
	CLock lock(&m_mutex);
	delete m_socketFactory;
	m_socketFactory = adopted;
}

void
CServer::setStreamFilterFactory(IStreamFilterFactory* adopted)
{
	CLock lock(&m_mutex);
	delete m_streamFilterFactory;
	m_streamFilterFactory = adopted;
}

CString
CServer::getPrimaryScreenName() const
{
	return m_name;
}

void
CServer::getConfig(CConfig* config) const
{
	assert(config != NULL);

	CLock lock(&m_mutex);
	*config = m_config;
}

UInt32
CServer::getActivePrimarySides() const
{
	// note -- m_mutex must be locked on entry
	UInt32 sides = 0;
	if (!m_config.getNeighbor(getPrimaryScreenName(), kLeft).empty()) {
		sides |= kLeftMask;
	}
	if (!m_config.getNeighbor(getPrimaryScreenName(), kRight).empty()) {
		sides |= kRightMask;
	}
	if (!m_config.getNeighbor(getPrimaryScreenName(), kTop).empty()) {
		sides |= kTopMask;
	}
	if (!m_config.getNeighbor(getPrimaryScreenName(), kBottom).empty()) {
		sides |= kBottomMask;
	}
	return sides;
}

void
CServer::onError()
{
	// stop all running threads but don't wait too long since some
	// threads may be unable to proceed until this thread returns.
	stopThreads(3.0);

	// done with the HTTP server
	CLock lock(&m_mutex);
	delete m_httpServer;
	m_httpServer = NULL;

	// note -- we do not attempt to close down the primary screen
}

void
CServer::onInfoChanged(const CString& name, const CClientInfo& info)
{
	CLock lock(&m_mutex);

	// look up client
	CClientList::iterator index = m_clients.find(name);
	if (index == m_clients.end()) {
		throw XBadClient();
	}
	IClient* client = index->second;
	assert(client != NULL);

	// update the remote mouse coordinates
	if (client == m_active) {
		m_x = info.m_mx;
		m_y = info.m_my;
	}
	log((CLOG_INFO "screen \"%s\" shape=%d,%d %dx%d zone=%d pos=%d,%d", name.c_str(), info.m_x, info.m_y, info.m_w, info.m_h, info.m_zoneSize, info.m_mx, info.m_my));

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimaryNoLock(m_x, m_y);
		}
		else {
			onMouseMoveSecondaryNoLock(0, 0);
		}
	}
}

bool
CServer::onGrabClipboard(const CString& name, ClipboardID id, UInt32 seqNum)
{
	CLock lock(&m_mutex);

	// screen must be connected
	CClientList::iterator grabber = m_clients.find(name);
	if (grabber == m_clients.end()) {
		throw XBadClient();
	}

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	CClipboardInfo& clipboard = m_clipboards[id];
	if (name != m_primaryClient->getName() &&
		seqNum < clipboard.m_clipboardSeqNum) {
		log((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", name.c_str(), id));
		return false;
	}

	// mark screen as owning clipboard
	log((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", name.c_str(), id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = name;
	clipboard.m_clipboardSeqNum = seqNum;

	// clear the clipboard data (since it's not known at this point)
	if (clipboard.m_clipboard.open(0)) {
		clipboard.m_clipboard.empty();
		clipboard.m_clipboard.close();
	}
	clipboard.m_clipboardData = clipboard.m_clipboard.marshall();

	// tell all other screens to take ownership of clipboard.  tell the
	// grabber that it's clipboard isn't dirty.
	for (CClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		if (index == grabber) {
			client->setClipboardDirty(id, false);
		}
		else {
			client->grabClipboard(id);
		}
	}

	return true;
}

void
CServer::onClipboardChanged(ClipboardID id, UInt32 seqNum, const CString& data)
{
	CLock lock(&m_mutex);
	onClipboardChangedNoLock(id, seqNum, data);
}

void
CServer::onClipboardChangedNoLock(ClipboardID id,
				UInt32 seqNum, const CString& data)
{
	CClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		log((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// ignore if data hasn't changed
	if (data == clipboard.m_clipboardData) {
		log((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// unmarshall into our clipboard buffer
	log((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	clipboard.m_clipboardData = data;
	clipboard.m_clipboard.unmarshall(clipboard.m_clipboardData, 0);

	// tell all clients except the sender that the clipboard is dirty
	CClientList::const_iterator sender =
								m_clients.find(clipboard.m_clipboardOwner);
	for (CClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		client->setClipboardDirty(id, index != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, m_clipboards[id].m_clipboardData);
}

void
CServer::onScreensaver(bool activated)
{
	log((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));
	CLock lock(&m_mutex);

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			switchScreen(m_primaryClient, 0, 0, true);
		}
	}
	else {
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != NULL && m_activeSaver != m_primaryClient) {
			// check position
			IClient* screen = m_activeSaver;
			SInt32 x, y, w, h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = screen->getJumpZoneSize();
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			switchScreen(screen, m_xSaver, m_ySaver, false);
		}

		// reset state
		m_activeSaver = NULL;
	}

	// send message to all clients
	for (CClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		client->screensaver(activated);
	}
}

void
CServer::onKeyDown(KeyID id, KeyModifierMask mask)
{
	log((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x", id, mask));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, true)) {
		return;
	}

	// relay
	m_active->keyDown(id, mask);
}

void
CServer::onKeyUp(KeyID id, KeyModifierMask mask)
{
	log((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x", id, mask));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, false)) {
		return;
	}

	// relay
	m_active->keyUp(id, mask);
}

void
CServer::onKeyRepeat(KeyID id, KeyModifierMask mask, SInt32 count)
{
	log((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d", id, mask, count));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, false)) {
		onCommandKey(id, mask, true);
		return;
	}

	// relay
	m_active->keyRepeat(id, mask, count);
}

void
CServer::onMouseDown(ButtonID id)
{
	log((CLOG_DEBUG1 "onMouseDown id=%d", id));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// relay
	m_active->mouseDown(id);
}

void
CServer::onMouseUp(ButtonID id)
{
	log((CLOG_DEBUG1 "onMouseUp id=%d", id));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// relay
	m_active->mouseUp(id);
}

bool
CServer::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	log((CLOG_DEBUG2 "onMouseMovePrimary %d,%d", x, y));
	CLock lock(&m_mutex);
	return onMouseMovePrimaryNoLock(x, y);
}

bool
CServer::onMouseMovePrimaryNoLock(SInt32 x, SInt32 y)
{
	// mouse move on primary (server's) screen
	assert(m_primaryClient != NULL);
	assert(m_active == m_primaryClient);

	// ignore if mouse is locked to screen
	if (isLockedToScreenNoLock()) {
		return false;
	}

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	SInt32 zoneSize = m_active->getJumpZoneSize();

	// see if we should change screens
	EDirection dir;
	if (x < ax + zoneSize) {
		x  -= zoneSize;
		dir = kLeft;
		log((CLOG_DEBUG1 "switch to left"));
	}
	else if (x >= ax + aw - zoneSize) {
		x  += zoneSize;
		dir = kRight;
		log((CLOG_DEBUG1 "switch to right"));
	}
	else if (y < ay + zoneSize) {
		y  -= zoneSize;
		dir = kTop;
		log((CLOG_DEBUG1 "switch to top"));
	}
	else if (y >= ay + ah - zoneSize) {
		y  += zoneSize;
		dir = kBottom;
		log((CLOG_DEBUG1 "switch to bottom"));
	}
	else {
		// still on local screen
		return false;
	}

	// get jump destination and, if no screen in jump direction,
	// then ignore the move.
	IClient* newScreen = getNeighbor(m_active, dir, x, y);
	if (newScreen == NULL) {
		return false;
	}

	// switch screen
	switchScreen(newScreen, x, y, false);
	return true;
}

void
CServer::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	log((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));
	CLock lock(&m_mutex);
	onMouseMoveSecondaryNoLock(dx, dy);
}

void
CServer::onMouseMoveSecondaryNoLock(SInt32 dx, SInt32 dy)
{
	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// we're actually on the primary screen.  this can happen
		// when the primary screen begins processing a mouse move
		// for a secondary screen, then the active (secondary)
		// screen disconnects causing us to jump to the primary
		// screen, and finally the primary screen finishes
		// processing the mouse move, still thinking it's for
		// a secondary screen.  we just ignore the motion.
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// accumulate motion
	m_x += dx;
	m_y += dy;

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);

	// switch screens if the mouse is outside the screen and not
	// locked to the screen
	IClient* newScreen = NULL;
	if (!isLockedToScreenNoLock()) {
		// find direction of neighbor
		EDirection dir;
		if (m_x < ax) {
			dir = kLeft;
		}
		else if (m_x > ax + aw - 1) {
			dir = kRight;
		}
		else if (m_y < ay) {
			dir = kTop;
		}
		else if (m_y > ay + ah - 1) {
			dir = kBottom;
		}
		else {
			newScreen = m_active;

			// keep compiler quiet about unset variable
			dir = kLeft;
		}

		// get neighbor if we should switch
		if (newScreen == NULL) {
			log((CLOG_DEBUG1 "leave \"%s\" on %s", m_active->getName().c_str(), CConfig::dirName(dir)));

			// get new position or clamp to current screen
			newScreen = getNeighbor(m_active, dir, m_x, m_y);
			if (newScreen == NULL) {
				log((CLOG_DEBUG1 "no neighbor; clamping"));
				if (m_x < ax) {
					m_x = ax;
				}
				else if (m_x > ax + aw - 1) {
					m_x = ax + aw - 1;
				}
				if (m_y < ay) {
					m_y = ay;
				}
				else if (m_y > ay + ah - 1) {
					m_y = ay + ah - 1;
				}
			}
		}
	}
	else {
		// clamp to edge when locked
		log((CLOG_DEBUG1 "clamp to \"%s\"", m_active->getName().c_str()));
		if (m_x < ax) {
			m_x = ax;
		}
		else if (m_x > ax + aw - 1) {
			m_x = ax + aw - 1;
		}
		if (m_y < ay) {
			m_y = ay;
		}
		else if (m_y > ay + ah - 1) {
			m_y = ay + ah - 1;
		}
	}

	// warp cursor if on same screen
	if (newScreen == NULL || newScreen == m_active) {
		// do nothing if mouse didn't move
		if (m_x != xOld || m_y != yOld) {
			log((CLOG_DEBUG2 "move on %s to %d,%d", m_active->getName().c_str(), m_x, m_y));
			m_active->mouseMove(m_x, m_y);
		}
	}

	// otherwise screen screens
	else {
		switchScreen(newScreen, m_x, m_y, false);
	}
}

void
CServer::onMouseWheel(SInt32 delta)
{
	log((CLOG_DEBUG1 "onMouseWheel %+d", delta));
	CLock lock(&m_mutex);
	assert(m_active != NULL);

	// relay
	m_active->mouseWheel(delta);
}

bool
CServer::onCommandKey(KeyID /*id*/, KeyModifierMask /*mask*/, bool /*down*/)
{
	return false;
}

bool
CServer::isLockedToScreenNoLock() const
{
	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// locked if scroll-lock is toggled on
	if ((m_primaryClient->getToggleMask() & KeyModifierScrollLock) != 0) {
		return true;
	}

	// not locked
	return false;
}

void
CServer::switchScreen(IClient* dst, SInt32 x, SInt32 y, bool forScreensaver)
{
	// note -- must be locked on entry

	assert(dst != NULL);
#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	log((CLOG_INFO "switch from \"%s\" to \"%s\" at %d,%d", m_active->getName().c_str(), dst->getName().c_str(), x, y));

	// record new position
	m_x = x;
	m_y = y;

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		// leave active screen
		if (!m_active->leave()) {
			// cannot leave screen
			log((CLOG_WARN "can't leave screen"));
			return;
		}

		// update the primary client's clipboards if we're leaving the
		// primary screen.
		if (m_active == m_primaryClient) {
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				CClipboardInfo& clipboard = m_clipboards[id];
				if (clipboard.m_clipboardOwner == m_primaryClient->getName()) {
					CString clipboardData;
					m_primaryClient->getClipboard(id, clipboardData);
					onClipboardChangedNoLock(id,
								clipboard.m_clipboardSeqNum, clipboardData);
				}
			}
		}

		// cut over
		m_active = dst;

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);

		// send the clipboard data to new active screen
		for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
			m_active->setClipboard(id, m_clipboards[id].m_clipboardData);
		}
	}
	else {
		m_active->mouseMove(x, y);
	}
}

IClient*
CServer::getNeighbor(IClient* src, EDirection dir) const
{
	assert(src != NULL);

	CString srcName = src->getName();
	assert(!srcName.empty());
	log((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", CConfig::dirName(dir), srcName.c_str()));
	for (;;) {
		// look up name of neighbor
		const CString dstName(m_config.getNeighbor(srcName, dir));

		// if nothing in that direction then return NULL
		if (dstName.empty()) {
			log((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", CConfig::dirName(dir), srcName.c_str()));
			return NULL;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.  otherwise we skip over an
		// unconnected screen.
		CClientList::const_iterator index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			log((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\"", dstName.c_str(), CConfig::dirName(dir), srcName.c_str()));
			return index->second;
		}

		log((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), CConfig::dirName(dir), srcName.c_str()));
		srcName = dstName;
	}
}

IClient*
CServer::getNeighbor(IClient* src,
				EDirection srcSide, SInt32& x, SInt32& y) const
{
	assert(src != NULL);

	// get the first neighbor
	IClient* dst = getNeighbor(src, srcSide);
	if (dst == NULL) {
		return NULL;
	}

	// get the source screen's size (needed for kRight and kBottom)
	SInt32 sx, sy, sw, sh;
	SInt32 dx, dy, dw, dh;
	IClient* lastGoodScreen = src;
	lastGoodScreen->getShape(sx, sy, sw, sh);
	lastGoodScreen->getShape(dx, dy, dw, dh);

	// find destination screen, adjusting x or y (but not both).  the
	// searches are done in a sort of canonical screen space where
	// the upper-left corner is 0,0 for each screen.  we adjust from
	// actual to canonical position on entry to and from canonical to
	// actual on exit from the search.
	switch (srcSide) {
	case kLeft:
		x -= dx;
		while (dst != NULL && dst != lastGoodScreen) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			x += dw;
			if (x >= 0) {
				break;
			}
			log((CLOG_DEBUG2 "skipping over screen %s", dst->getName().c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kRight:
		x -= dx;
		while (dst != NULL) {
			x -= dw;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (x < dw) {
				break;
			}
			log((CLOG_DEBUG2 "skipping over screen %s", dst->getName().c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kTop:
		y -= dy;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			y += dh;
			if (y >= 0) {
				break;
			}
			log((CLOG_DEBUG2 "skipping over screen %s", dst->getName().c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kBottom:
		y -= dy;
		while (dst != NULL) {
			y -= dh;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (y < sh) {
				break;
			}
			log((CLOG_DEBUG2 "skipping over screen %s", dst->getName().c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;
	}

	// save destination screen
	assert(lastGoodScreen != NULL);
	dst = lastGoodScreen;

	// if entering primary screen then be sure to move in far enough
	// to avoid the jump zone.  if entering a side that doesn't have
	// a neighbor (i.e. an asymmetrical side) then we don't need to
	// move inwards because that side can't provoke a jump.
	if (dst == m_primaryClient) {
		const CString dstName(dst->getName());
		switch (srcSide) {
		case kLeft:
			if (!m_config.getNeighbor(dstName, kRight).empty() &&
				x > dx + dw - 1 - dst->getJumpZoneSize())
				x = dx + dw - 1 - dst->getJumpZoneSize();
			break;

		case kRight:
			if (!m_config.getNeighbor(dstName, kLeft).empty() &&
				x < dx + dst->getJumpZoneSize())
				x = dx + dst->getJumpZoneSize();
			break;

		case kTop:
			if (!m_config.getNeighbor(dstName, kBottom).empty() &&
				y > dy + dh - 1 - dst->getJumpZoneSize())
				y = dy + dh - 1 - dst->getJumpZoneSize();
			break;

		case kBottom:
			if (!m_config.getNeighbor(dstName, kTop).empty() &&
				y < dy + dst->getJumpZoneSize())
				y = dy + dst->getJumpZoneSize();
			break;
		}
	}

	// adjust the coordinate orthogonal to srcSide to account for
	// resolution differences.  for example, if y is 200 pixels from
	// the top on a screen 1000 pixels high (20% from the top) when
	// we cross the left edge onto a screen 600 pixels high then y
	// should be set 120 pixels from the top (again 20% from the
	// top).
	switch (srcSide) {
	case kLeft:
	case kRight:
		y -= sy;
		if (y < 0) {
			y = 0;
		}
		else if (y >= sh) {
			y = dh - 1;
		}
		else {
			y = static_cast<SInt32>(0.5 + y *
								static_cast<double>(dh - 1) / (sh - 1));
		}
		y += dy;
		break;

	case kTop:
	case kBottom:
		x -= sx;
		if (x < 0) {
			x = 0;
		}
		else if (x >= sw) {
			x = dw - 1;
		}
		else {
			x = static_cast<SInt32>(0.5 + x *
								static_cast<double>(dw - 1) / (sw - 1));
		}
		x += dx;
		break;
	}

	return dst;
}

void
CServer::closeClients(const CConfig& config)
{
	CThreadList threads;
	{
		CLock lock(&m_mutex);

		// get the set of clients that are connected but are being
		// dropped from the configuration (or who's canonical name
		// is changing) and tell them to disconnect.  note that
		// since m_clientThreads doesn't include a thread for the
		// primary client we will not close it.
		for (CClientThreadList::iterator
								index  = m_clientThreads.begin();
								index != m_clientThreads.end(); ) {
			const CString& name = index->first;
			if (!config.isCanonicalName(name)) {
				// lookup IClient with name
				CClientList::const_iterator index2 = m_clients.find(name);
				assert(index2 != m_clients.end());

				// save the thread and remove it from m_clientThreads
				threads.push_back(index->second);
				m_clientThreads.erase(index++);

				// close that client
				assert(index2->second != m_primaryClient);
				index2->second->close();
			}
			else {
				++index;
			}
		}
	}

	// wait a moment to allow each client to close its connection
	// before we close it (to avoid having our socket enter TIME_WAIT).
	if (threads.size() > 0) {
		CThread::sleep(1.0);
	}

	// cancel the old client threads
	for (CThreadList::iterator index = threads.begin();
								index != threads.end(); ++index) {
		index->cancel();
	}

	// wait for old client threads to terminate.  we must not hold
	// the lock while we do this so those threads can finish any
	// calls to this object.
	for (CThreadList::iterator index = threads.begin();
								index != threads.end(); ++index) {
		index->wait();
	}

	// clean up thread list
	reapThreads();
}

CThread
CServer::startThread(IJob* job)
{
	CLock lock(&m_mutex);

	// reap completed threads
	doReapThreads(m_threads);

	// add new thread to list.  use the job as user data for logging.
	CThread thread(job, job);
	m_threads.push_back(thread);
	log((CLOG_DEBUG1 "started thread %p", thread.getUserData()));
	return thread;
}

void
CServer::stopThreads(double timeout)
{
	log((CLOG_DEBUG1 "stopping threads"));

	// cancel the accept client thread to prevent more clients from
	// connecting while we're shutting down.
	CThread* acceptClientThread;
	{
		CLock lock(&m_mutex);
		acceptClientThread   = m_acceptClientThread;
		m_acceptClientThread = NULL;
	}
	if (acceptClientThread != NULL) {
		acceptClientThread->cancel();
		acceptClientThread->wait(timeout);
		delete acceptClientThread;
	}

	// close all clients (except the primary)
	{
		CConfig emptyConfig;
		closeClients(emptyConfig);
	}

	// swap thread list so nobody can mess with it
	CThreadList threads;
	{
		CLock lock(&m_mutex);
		threads.swap(m_threads);
	}

	// cancel every thread
	for (CThreadList::iterator index = threads.begin();
								index != threads.end(); ++index) {
		index->cancel();
	}

	// now wait for the threads
	CStopwatch timer(true);
	while (threads.size() > 0 && (timeout < 0.0 || timer.getTime() < timeout)) {
		doReapThreads(threads);
		CThread::sleep(0.01);
	}

	// delete remaining threads
	for (CThreadList::iterator index = threads.begin();
								index != threads.end(); ++index) {
		log((CLOG_DEBUG1 "reaped running thread %p", index->getUserData()));
	}

	log((CLOG_DEBUG1 "stopped threads"));
}

void
CServer::reapThreads()
{
	CLock lock(&m_mutex);
	doReapThreads(m_threads);
}

void
CServer::doReapThreads(CThreadList& threads)
{
	for (CThreadList::iterator index = threads.begin();
								index != threads.end(); ) {
		if (index->wait(0.0)) {
			// thread terminated
			log((CLOG_DEBUG1 "reaped thread %p", index->getUserData()));
			index = threads.erase(index);
		}
		else {
			// thread is running
			++index;
		}
	}
}

void
CServer::acceptClients(void*)
{
	log((CLOG_DEBUG1 "starting to wait for clients"));

	IListenSocket* listen = NULL;
	try {
		// create socket listener
		if (m_socketFactory != NULL) {
			listen = m_socketFactory->createListen();
		}
		assert(listen != NULL);

		// bind to the desired port.  keep retrying if we can't bind
		// the address immediately.
		CStopwatch timer;
		for (;;) {
			try {
				log((CLOG_DEBUG1 "binding listen socket"));
				listen->bind(m_config.getSynergyAddress());
				break;
			}
			catch (XSocketBind& e) {
				log((CLOG_DEBUG1 "bind failed: %s", e.getErrstr()));

				// give up if we've waited too long
				if (timer.getTime() >= m_bindTimeout) {
					log((CLOG_DEBUG1 "waited too long to bind, giving up"));
					throw;
				}

				// wait a bit before retrying
				CThread::sleep(5.0);
			}
		}

		// accept connections and begin processing them
		log((CLOG_DEBUG1 "waiting for client connections"));
		for (;;) {
			// accept connection
			CThread::testCancel();
			IDataSocket* socket = listen->accept();
			log((CLOG_NOTE "accepted client connection"));
			CThread::testCancel();

			// start handshake thread
			startThread(new TMethodJob<CServer>(
								this, &CServer::runClient, socket));
		}

		// clean up
		delete listen;
	}
	catch (XBase& e) {
		log((CLOG_ERR "cannot listen for clients: %s", e.what()));
		delete listen;
		exitMainLoop();
	}
	catch (...) {
		delete listen;
		throw;
	}
}

void
CServer::runClient(void* vsocket)
{
	// get the socket pointer from the argument
	assert(vsocket != NULL);
	IDataSocket* socket = reinterpret_cast<IDataSocket*>(vsocket);

	// create proxy
	CClientProxy* proxy = NULL;
	try {
		proxy = handshakeClient(socket);
		if (proxy == NULL) {
			delete socket;
			return;
		}
	}
	catch (...) {
		delete socket;
		throw;
	}

	// add the connection
	try {
		addConnection(proxy);

		// save this client's thread
		CLock lock(&m_mutex);
		m_clientThreads.insert(std::make_pair(proxy->getName(),
								CThread::getCurrentThread()));
	}
	catch (XDuplicateClient& e) {
		// client has duplicate name
		log((CLOG_WARN "a client with name \"%s\" is already connected", e.getName().c_str()));
		CProtocolUtil::writef(proxy->getOutputStream(), kMsgEBusy);
		delete proxy;
		delete socket;
		return;
	}
	catch (XUnknownClient& e) {
		// client has unknown name
		log((CLOG_WARN "a client with name \"%s\" is not in the map", e.getName().c_str()));
		CProtocolUtil::writef(proxy->getOutputStream(), kMsgEUnknown);
		delete proxy;
		delete socket;
		return;
	}
	catch (...) {
		delete proxy;
		delete socket;
		throw;
	}

	// activate screen saver on new client if active on the primary screen
	{
		CLock lock(&m_mutex);
		if (m_activeSaver != NULL) {
			proxy->screensaver(true);
		}
	}

	// handle client messages
	try {
		log((CLOG_NOTE "client \"%s\" has connected", proxy->getName().c_str()));
		proxy->mainLoop();
	}
	catch (XBadClient&) {
		// client not behaving
		log((CLOG_WARN "protocol error from client \"%s\"", proxy->getName().c_str()));
		CProtocolUtil::writef(proxy->getOutputStream(), kMsgEBad);
	}
	catch (XBase& e) {
		// misc error
		log((CLOG_WARN "error communicating with client \"%s\": %s", proxy->getName().c_str(), e.what()));
	}
	catch (...) {
		// mainLoop() was probably cancelled
		removeConnection(proxy->getName());
		delete socket;
		throw;
	}

	// clean up
	removeConnection(proxy->getName());
	delete socket;
}

CClientProxy*
CServer::handshakeClient(IDataSocket* socket)
{
	log((CLOG_DEBUG1 "negotiating with new client"));

	// get the input and output streams
	IInputStream*  input  = socket->getInputStream();
	IOutputStream* output = socket->getOutputStream();
	bool own              = false;

	// attach filters
	if (m_streamFilterFactory != NULL) {
		input  = m_streamFilterFactory->createInput(input, own);
		output = m_streamFilterFactory->createOutput(output, own);
		own    = true;
	}

	// attach the packetizing filters
	input  = new CInputPacketStream(input, own);
	output = new COutputPacketStream(output, own);
	own    = true;

	CClientProxy* proxy = NULL;
	CString name("<unknown>");
	try {
		// give the client a limited time to complete the handshake
		CTimerThread timer(30.0);

		// say hello
		log((CLOG_DEBUG1 "saying hello"));
		CProtocolUtil::writef(output, kMsgHello,
										kProtocolMajorVersion,
										kProtocolMinorVersion);
		output->flush();

		// wait for the reply
		log((CLOG_DEBUG1 "waiting for hello reply"));
		UInt32 n = input->getSize();

		// limit the maximum length of the hello
		if (n > kMaxHelloLength) {
			throw XBadClient();
		}

		// get and parse the reply to hello
		SInt16 major, minor;
		try {
			log((CLOG_DEBUG1 "parsing hello reply"));
			CProtocolUtil::readf(input, kMsgHelloBack,
										&major, &minor, &name);
		}
		catch (XIO&) {
			throw XBadClient();
		}

		// disallow invalid version numbers
		if (major < 0 || minor < 0) {
			throw XIncompatibleClient(major, minor);
		}

		// disallow connection from test versions to release versions
		if (major == 0 && kProtocolMajorVersion != 0) {
			throw XIncompatibleClient(major, minor);
		}

		// hangup (with error) if version isn't supported
		if (major > kProtocolMajorVersion ||
			(major == kProtocolMajorVersion && minor > kProtocolMinorVersion)) {
			throw XIncompatibleClient(major, minor);
		}

		// convert name to canonical form (if any)
		if (m_config.isScreen(name)) {
			name = m_config.getCanonicalName(name);
		}

		// create client proxy for highest version supported by the client
		log((CLOG_DEBUG1 "creating proxy for client \"%s\" version %d.%d", name.c_str(), major, minor));
		proxy = new CClientProxy1_0(this, name, input, output);

		// negotiate
		// FIXME

		// ask and wait for the client's info
		log((CLOG_DEBUG1 "waiting for info for client \"%s\"", name.c_str()));
		proxy->open();

		return proxy;
	}
	catch (XIncompatibleClient& e) {
		// client is incompatible
		log((CLOG_WARN "client \"%s\" has incompatible version %d.%d)", name.c_str(), e.getMajor(), e.getMinor()));
		CProtocolUtil::writef(output, kMsgEIncompatible,
							kProtocolMajorVersion, kProtocolMinorVersion);
	}
	catch (XBadClient&) {
		// client not behaving
		log((CLOG_WARN "protocol error from client \"%s\"", name.c_str()));
		CProtocolUtil::writef(output, kMsgEBad);
	}
	catch (XBase& e) {
		// misc error
		log((CLOG_WARN "error communicating with client \"%s\": %s", name.c_str(), e.what()));
	}
	catch (...) {
		// probably timed out
		if (proxy != NULL) {
			delete proxy;
		}
		else if (own) {
			delete input;
			delete output;
		}
		throw;
	}

	// failed
	if (proxy != NULL) {
		delete proxy;
	}
	else if (own) {
		delete input;
		delete output;
	}

	return NULL;
}

void
CServer::acceptHTTPClients(void*)
{
	log((CLOG_DEBUG1 "starting to wait for HTTP clients"));

	IListenSocket* listen = NULL;
	try {
		// create socket listener
		listen = new CTCPListenSocket;

		// bind to the desired port.  keep retrying if we can't bind
		// the address immediately.
		CStopwatch timer;
		for (;;) {
			try {
				log((CLOG_DEBUG1 "binding HTTP listen socket"));
				listen->bind(m_config.getHTTPAddress());
				break;
			}
			catch (XSocketBind& e) {
				log((CLOG_DEBUG1 "bind HTTP failed: %s", e.getErrstr()));

				// give up if we've waited too long
				if (timer.getTime() >= m_bindTimeout) {
					log((CLOG_DEBUG1 "waited too long to bind HTTP, giving up"));
					throw;
				}

				// wait a bit before retrying
				CThread::sleep(5.0);
			}
		}

		// accept connections and begin processing them
		log((CLOG_DEBUG1 "waiting for HTTP connections"));
		for (;;) {
			// limit the number of HTTP requests being handled at once
			{
				CLock lock(&m_httpAvailable);
				while (m_httpAvailable == 0) {
					m_httpAvailable.wait();
				}
				assert(m_httpAvailable > 0);
				m_httpAvailable = m_httpAvailable - 1;
			}

			// accept connection
			CThread::testCancel();
			IDataSocket* socket = listen->accept();
			log((CLOG_NOTE "accepted HTTP connection"));
			CThread::testCancel();

			// handle HTTP request
			startThread(new TMethodJob<CServer>(
								this, &CServer::processHTTPRequest, socket));
		}

		// clean up
		delete listen;
	}
	catch (XBase& e) {
		log((CLOG_ERR "cannot listen for HTTP clients: %s", e.what()));
		delete listen;
		exitMainLoop();
	}
	catch (...) {
		delete listen;
		throw;
	}
}

void
CServer::processHTTPRequest(void* vsocket)
{
	IDataSocket* socket = reinterpret_cast<IDataSocket*>(vsocket);
	try {
		// process the request and force delivery
		m_httpServer->processRequest(socket);
		socket->getOutputStream()->flush();

		// wait a moment to give the client a chance to hangup first
		CThread::sleep(3.0);

		// clean up
		socket->close();
		delete socket;

		// increment available HTTP handlers
		{
			CLock lock(&m_httpAvailable);
			m_httpAvailable = m_httpAvailable + 1;
			m_httpAvailable.signal();
		}
	}
	catch (...) {
		delete socket;
		{
			CLock lock(&m_httpAvailable);
			m_httpAvailable = m_httpAvailable + 1;
			m_httpAvailable.signal();
		}
		throw;
	}
}

void
CServer::openPrimaryScreen()
{
	assert(m_primaryClient == NULL);

	// reset sequence number
	m_seqNum = 0;

	// canonicalize the primary screen name
	CString primaryName = m_config.getCanonicalName(getPrimaryScreenName());
	if (primaryName.empty()) {
		throw XUnknownClient(getPrimaryScreenName());
	}

	// clear clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		CClipboardInfo& clipboard   = m_clipboards[id];
		clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData   = clipboard.m_clipboard.marshall();
	}

	try {
		// create the primary client
		m_primaryClient = new CPrimaryClient(m_screenFactory,
								this, this, primaryName);

		// add connection
		addConnection(m_primaryClient);
		m_active = m_primaryClient;

		// open the screen
		log((CLOG_DEBUG1 "opening primary screen"));
		m_primaryClient->open();

		// tell it about the active sides
		m_primaryClient->reconfigure(getActivePrimarySides());
	}
	catch (...) {
		if (m_active != NULL) {
			removeConnection(primaryName);
		}
		else {
			delete m_primaryClient;
		}
		m_active        = NULL;
		m_primaryClient = NULL;
		throw;
	}
}

void
CServer::closePrimaryScreen()
{
	assert(m_primaryClient != NULL);

	// close the primary screen
	try {
		log((CLOG_DEBUG1 "closing primary screen"));
		m_primaryClient->close();
	}
	catch (...) {
		// ignore
	}

	// remove connection
	removeConnection(m_primaryClient->getName());
	m_primaryClient = NULL;
}

void
CServer::addConnection(IClient* client)
{
	assert(client != NULL);

	log((CLOG_DEBUG "adding connection \"%s\"", client->getName().c_str()));

	CLock lock(&m_mutex);

	// name must be in our configuration
	if (!m_config.isScreen(client->getName())) {
		throw XUnknownClient(client->getName());
	}

	// can only have one screen with a given name at any given time
	if (m_clients.count(client->getName()) != 0) {
		throw XDuplicateClient(client->getName());
	}

	// save screen info
	m_clients.insert(std::make_pair(client->getName(), client));
	log((CLOG_DEBUG "added connection \"%s\"", client->getName().c_str()));
}

void
CServer::removeConnection(const CString& name)
{
	log((CLOG_DEBUG "removing connection \"%s\"", name.c_str()));
	CLock lock(&m_mutex);

	// find client
	CClientList::iterator index = m_clients.find(name);
	assert(index != m_clients.end());

	// if this is active screen then we have to jump off of it
	IClient* active = (m_activeSaver != NULL) ? m_activeSaver : m_active;
	if (active == index->second && active != m_primaryClient) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);

		// don't notify active screen since it probably already disconnected
		log((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", active->getName().c_str(), m_primaryClient->getName().c_str(), m_x, m_y));

		// cut over
		m_active = m_primaryClient;

		// enter new screen (unless we already have because of the
		// screen saver)
		if (m_activeSaver == NULL) {
			m_primaryClient->enter(m_x, m_y, m_seqNum,
								m_primaryClient->getToggleMask(), false);
		}
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == index->second) {
		m_activeSaver = NULL;
	}

	// done with client
	delete index->second;
	m_clients.erase(index);

	// remove any thread for this client
	m_clientThreads.erase(name);
}


//
// CServer::CClipboardInfo
//

CServer::CClipboardInfo::CClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0)
{
	// do nothing
}