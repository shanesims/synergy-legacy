/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2002 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef IPLATFORMSCREEN_H
#define IPLATFORMSCREEN_H

#include "IPrimaryScreen.h"
#include "ISecondaryScreen.h"
#include "ClipboardTypes.h"
#include "OptionTypes.h"

class IClipboard;
class IKeyState;

//! Screen interface
/*!
This interface defines the methods common to all platform dependent
screen implementations that are used by both primary and secondary
screens.
*/
class IPlatformScreen : public IPrimaryScreen, public ISecondaryScreen {
public:
	//! @name manipulators
	//@{

	//! Open screen
	/*!
	Called to open and initialize the screen.  Throw XScreenUnavailable
	if the screen cannot be opened but retrying later may succeed.
	Otherwise throw some other XScreenOpenFailure exception.
	*/
	virtual void		open(IKeyState*) = 0;

	//! Close screen
	/*!
	Called to close the screen.  close() should quietly ignore calls
	that don't have a matching successful call to open().
	*/
	virtual void		close() = 0;

	//! Enable screen
	/*!
	Enable the screen, preparing it to report system and user events.
	For a secondary screen it also means preparing to synthesize events
	and hiding the cursor.
	*/
	virtual void		enable() = 0;

	//! Disable screen
	/*!
	Undoes the operations in enable() and events should no longer
	be reported.
	*/
	virtual void		disable() = 0;

	//! Run event loop
	/*!
	Run the event loop and return when exitMainLoop() is called.
	This must be called between a successful open() and close().
	*/
	virtual void		mainLoop() = 0;

	//! Exit event loop
	/*!
	Force mainLoop() to return.  This call can return before
	mainLoop() does (i.e. asynchronously).  This may only be
	called between a successful open() and close().
	*/
	virtual void		exitMainLoop() = 0;

	//! Enter screen
	/*!
	Called when the user navigates to this screen.
	*/
	virtual void		enter() = 0;

	//! Leave screen
	/*!
	Called when the user navigates off the screen.  Returns true on
	success, false on failure.  A typical reason for failure is being
	unable to install the keyboard and mouse snoopers on a primary
	screen.  Secondary screens should not fail.
	*/
	virtual bool		leave() = 0;

	//! Set clipboard
	/*!
	Set the contents of the system clipboard indicated by \c id.
	*/
	virtual bool		setClipboard(ClipboardID id, const IClipboard*) = 0;

	//! Check clipboard owner
	/*!
	Check ownership of all clipboards and notify an IScreenReceiver (set
	through some other interface) if any changed.  This is used as a
	backup in case the system doesn't reliably report clipboard ownership
	changes.
	*/
	virtual void		checkClipboards() = 0;

	//! Open screen saver
	/*!
	Open the screen saver.  If \c notify is true then this object must
	call an IScreenEventHandler's (set through some other interface)
	onScreenSaver() when the screensaver activates or deactivates until
	it's closed.  If \c notify is false then the screen saver is
	disabled on open and restored on close.
	*/
// XXX -- pass an interface pointer, not a notify flag
	virtual void		openScreensaver(bool notify) = 0;

	//! Close screen saver
	/*!
	// Close the screen saver.  Stop reporting screen saver activation
	and deactivation and, if the screen saver was disabled by
	openScreensaver(), enable the screen saver.
	*/
	virtual void		closeScreensaver() = 0;

	//! Activate/deactivate screen saver
	/*!
	Forcibly activate the screen saver if \c activate is true otherwise
	forcibly deactivate it.
	*/
	virtual void		screensaver(bool activate) = 0;

	//! Notify of options changes
	/*!
	Reset all options to their default values.
	*/
	virtual void		resetOptions() = 0;

	//! Notify of options changes
	/*!
	Set options to given values.  Ignore unknown options and don't
	modify options that aren't given in \c options.
	*/
	virtual void		setOptions(const COptionsList& options) = 0;

	//! Get keyboard state
	/*!
	Put the current keyboard state into the IKeyState passed to \c open().
	*/
	virtual void		updateKeys() = 0;

	//@}
	//! @name accessors
	//@{

	//! Test if is primary screen
	/*!
	Return true iff this screen is a primary screen.
	*/
	virtual bool		isPrimary() const = 0;

	//! Get clipboard
	/*!
	Save the contents of the clipboard indicated by \c id and return
	true iff successful.
	*/
	virtual bool		getClipboard(ClipboardID id, IClipboard*) const = 0;

	//! Get screen shape
	/*!
	Return the position of the upper-left corner of the screen in \c x and
	\c y and the size of the screen in \c w (width) and \c h (height).
	*/
	virtual void		getShape(SInt32& x, SInt32& y,
							SInt32& w, SInt32& h) const = 0;

	//! Get cursor position
	/*!
	Return the current position of the cursor in \c x and \c y.
	*/
	virtual void		getCursorPos(SInt32& x, SInt32& y) const = 0;

	//@}

	// IPrimaryScreen overrides
	virtual void		reconfigure(UInt32 activeSides) = 0;
	virtual void		warpCursor(SInt32 x, SInt32 y) = 0;
	virtual UInt32		addOneShotTimer(double timeout) = 0;
	virtual SInt32		getJumpZoneSize() const = 0;
	virtual bool		isAnyMouseButtonDown() const = 0;
	virtual const char*	getKeyName(KeyButton) const = 0;

	// ISecondaryScreen overrides
	virtual void		fakeKeyEvent(KeyButton id, bool press) const = 0;
	virtual bool		fakeCtrlAltDel() const = 0;
	virtual void		fakeMouseButton(ButtonID id, bool press) const = 0;
	virtual void		fakeMouseMove(SInt32 x, SInt32 y) const = 0;
	virtual void		fakeMouseWheel(SInt32 delta) const = 0;
	virtual KeyButton	mapKey(IKeyState::Keystrokes&,
							const IKeyState& keyState, KeyID id,
							KeyModifierMask desiredMask,
							bool isAutoRepeat) const = 0;
};

#endif