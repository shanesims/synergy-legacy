#ifndef CXWINDOWSCLIPBOARD_H
#define CXWINDOWSCLIPBOARD_H

#include "IClipboard.h"
#include "ClipboardTypes.h"
#include "stdmap.h"
#include "stdlist.h"
#if defined(X_DISPLAY_MISSING)
#	error X11 is required to build synergy
#else
#	include <X11/Xlib.h>
#endif

class CXWindowsClipboard : public IClipboard {
public:
	CXWindowsClipboard(Display*, Window, ClipboardID);
	virtual ~CXWindowsClipboard();

	// tell clipboard it lost ownership
	void				lost(Time);

	// add a selection request to the request list.  if the given
	// owner window isn't this clipboard's window then this simply
	// sends a failure event to the requestor.
	void				addRequest(Window owner,
							Window requestor, Atom target,
							::Time time, Atom property);

	// continue processing a selection request.  returns true if the
	// request was handled, false if the request was unknown.
	bool				processRequest(Window requestor,
							::Time time, Atom property);

	// terminate a selection request.  returns true iff the request
	// was known and handled.
	bool				destroyRequest(Window requestor);

	// get the clipboard's window
	Window				getWindow() const;

	// get the clipboard selection atom
	Atom				getSelection() const;

	// IClipboard overrides
	virtual bool		empty();
	virtual void		add(EFormat, const CString& data);
	virtual bool		open(Time) const;
	virtual void		close() const;
	virtual Time		getTime() const;
	virtual bool		has(EFormat) const;
	virtual CString		get(EFormat) const;

private:
	// convert target atom to clipboard format
	EFormat				getFormat(Atom target) const;

	// add a non-MULTIPLE request.  does not verify that the selection
	// was owned at the given time.  returns true if the conversion
	// could be performed, false otherwise.  in either case, the
	// reply is inserted.
	bool				addSimpleRequest(
							Window requestor, Atom target,
							::Time time, Atom property);

	// clear the cache, resetting the cached flag and the added flag for
	// each format.
	void				clearCache() const;
	void				doClearCache();

	// cache all formats of the selection
	void				fillCache() const;
	void				doFillCache();

	// ICCCM interoperability methods
	void				icccmFillCache();
	bool				icccmGetSelection(Atom target,
							Atom* actualTarget,
							CString* data) const;
	Time				icccmGetTime() const;

	// motif interoperability methods
	bool				motifLockClipboard() const;
	void				motifUnlockClipboard() const;
	bool				motifOwnsClipboard() const;
	Time				motifGetTime() const;
	void				motifFillCache();
	// FIXME

	//
	// helper classes
	//

	// read an ICCCM conforming selection
	class CICCCMGetClipboard {
	public:
		CICCCMGetClipboard(Window requestor, Time time, Atom property);
		~CICCCMGetClipboard();

		// convert the given selection to the given type.  returns
		// true iff the conversion was successful or the conversion
		// cannot be performed (in which case *actualTarget == None).
		bool			readClipboard(Display* display,
							Atom selection, Atom target,
							Atom* actualTarget, CString* data);

	private:
		bool			doEventPredicate(Display* display,
							XEvent* event);
		static Bool		eventPredicate(Display* display,
							XEvent* event,
							XPointer arg);
		void			timeout(void*);

	private:
		Window			m_requestor;
		Time			m_time;
		Atom			m_property;
		bool			m_incr;
		bool			m_failed;
		bool			m_done;

		// true iff we've received the selection notify
		bool			m_reading;

		// the converted selection data
		CString*		m_data;

		// the actual type of the data.  if this is None then the
		// selection owner cannot convert to the requested type.
		Atom*			m_actualTarget;

		// property used in event to wake up event loop
		Atom			m_timeout;

	public:
		// true iff the selection owner didn't follow ICCCM conventions
		bool			m_error;
	};

	// Motif structure IDs
	enum { kMotifClipFormat = 1, kMotifClipItem, kMotifClipHeader };

	// _MOTIF_CLIP_HEADER structure
	class CMotifClipHeader {
	public:
		SInt32			m_id;			// kMotifClipHeader
		SInt32			m_pad1[3];
		SInt32			m_item;
		SInt32			m_pad2[4];
		SInt32			m_numItems;
		SInt32			m_pad3[3];
		Window			m_selectionOwner;
		SInt32			m_pad4[2];
		SInt32			m_items[1];		// m_numItems items
	};

	// Motif clip item structure
	class CMotifClipItem {
	public:
		SInt32			m_id;			// kMotifClipItem
		SInt32			m_pad1[6];
		SInt32			m_numFormats;
		SInt32			m_pad2[7];
		SInt32			m_formats[1];	// m_numFormats formats
	};

	// Motif clip format structure
	class CMotifClipFormat {
	public:
		SInt32			m_id;			// kMotifClipFormat
		SInt32			m_pad1[6];
		SInt32			m_length;
		SInt32			m_data;
		Atom			m_type;
		SInt32			m_pad2[6];
	};

	// stores data needed to respond to a selection request
	class CReply {
	public:
		CReply(Window, Atom target, ::Time);
		CReply(Window, Atom target, ::Time, Atom property,
							const CString& data, Atom type, int format);

	public:
		// information about the request
		Window			m_requestor;
		Atom			m_target;
		::Time			m_time;
		Atom			m_property;

		// true iff we've sent the notification for this reply
		bool			m_replied;

		// true iff the reply has sent its last message
		bool			m_done;

		// the data to send and its type and format
		CString			m_data;
		Atom			m_type;
		int				m_format;

		// index of next byte in m_data to send
		UInt32			m_ptr;
	};
	typedef std::list<CReply*> CReplyList;
	typedef std::map<Window, CReplyList> CReplyMap;
	typedef std::map<Window, long> CReplyEventMask;

	// reply methods
	bool				insertMultipleReply(Window, ::Time, Atom);
	void				insertReply(CReply*);
	void				pushReplies();
	void				pushReplies(CReplyMap::iterator,
							CReplyList&, CReplyList::iterator);
	bool				sendReply(CReply*);
	void				clearReplies();
	void				clearReplies(CReplyList&);
	void				sendNotify(Window requestor, Atom selection,
							Atom target, Atom property, Time time);
	bool				wasOwnedAtTime(::Time) const;

	// data conversion methods
	Atom				getTargetsData(CString&, int* format) const;
	Atom				getTimestampData(CString&, int* format) const;
	Atom				getStringData(CString&, int* format) const;

private:
	Display*			m_display;
	Window				m_window;
	ClipboardID			m_id;
	Atom				m_selection;
	mutable bool		m_open;
	mutable Time		m_time;
	bool				m_owner;
	mutable Time		m_timeOwned;
	Time				m_timeLost;

	// true iff open and clipboard owned by a motif app
	mutable bool		m_motif;

	// the added/cached clipboard data
	bool				m_cached;
	Time				m_cacheTime;
	bool				m_added[kNumFormats];
	CString				m_data[kNumFormats];

	// conversion request replies
	CReplyMap			m_replies;
	CReplyEventMask		m_eventMasks;

	// atoms we'll need
	Atom				m_atomTargets;
	Atom				m_atomMultiple;
	Atom				m_atomTimestamp;
	Atom				m_atomAtom;
	Atom				m_atomAtomPair;
	Atom				m_atomInteger;
	Atom				m_atomData;
	Atom				m_atomINCR;
	Atom				m_atomString;
	Atom				m_atomText;
	Atom				m_atomCompoundText;
	Atom				m_atomMotifClipLock;
	Atom				m_atomMotifClipHeader;
	Atom				m_atomMotifClipAccess;
	Atom				m_atomGDKSelection;
};

#endif