#ifndef CNETWORKADDRESS_H
#define CNETWORKADDRESS_H

#include "CNetwork.h"
#include "CString.h"
#include "BasicTypes.h"

//! Network address type
/*!
This class represents a network address.
*/
class CNetworkAddress {
public:
	/*!
	Constructs the invalid address
	*/
	CNetworkAddress();

	/*!
	Construct the wildcard address with the given port.  \c port must
	not be zero.
	*/
	CNetworkAddress(UInt16 port);

	/*!
	Construct the network address for the given \c hostname and \c port.
	If \c hostname can be parsed as a numerical address then that's how
	it's used, otherwise the host name is looked up.  If the lookup fails
	then this throws XSocketAddress.  If \c hostname ends in ":[0-9]+" then
	that suffix is extracted and used as the port, overridding the port
	parameter.  Neither the extracted port or \c port may be zero.
	*/
	CNetworkAddress(const CString& hostname, UInt16 port);

	~CNetworkAddress();

	//! @name accessors
	//@{

	//! Check address validity
	/*!
	Returns true if this is not the invalid address.
	*/
	bool				isValid() const;

	//! Get address
	/*!
	Returns the address in the platform's native network address
	structure.
	*/
	const CNetwork::Address*	getAddress() const;

	//! Get address length
	/*!
	Returns the length of the address in the platform's native network
	address structure.
	*/
	CNetwork::AddressLength		getAddressLength() const;

	//! Get hostname
	/*!
	Returns the hostname passed to the c'tor sans the port suffix.
	*/
	CString				getHostname() const;

	//! Get port
	/*!
	Returns the port passed to the c'tor as a suffix to the hostname,
	if that existed, otherwise as passed directly to the c'tor.
	*/
	UInt16				getPort() const;

	//@}

private:
	CNetwork::Address	m_address;
	CString				m_hostname;
	UInt16				m_port;
};

#endif