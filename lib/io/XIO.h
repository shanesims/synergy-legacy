#ifndef XIO_H
#define XIO_H

#include "XBase.h"

//! Generic I/O exception
class XIO : public XBase { };

//! Generic I/O exception using \c errno
class XIOErrno : public XIO, public MXErrno {
public:
	XIOErrno();
	XIOErrno(int);
};

//! I/O closing exception
/*!
Thrown if a stream cannot be closed.
*/
class XIOClose: public XIOErrno {
protected:
	// XBase overrides
	virtual CString		getWhat() const throw();
};

//! I/O already closed exception
/*!
Thrown when attempting to close or perform I/O on an already closed.
stream.
*/
class XIOClosed : public XIO {
protected:
	// XBase overrides
	virtual CString		getWhat() const throw();
};

//! I/O end of stream exception
/*!
Thrown when attempting to read beyond the end of a stream.
*/
class XIOEndOfStream : public XIO {
protected:
	// XBase overrides
	virtual CString		getWhat() const throw();
};

#endif