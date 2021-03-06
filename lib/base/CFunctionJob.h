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

#ifndef CFUNCTIONJOB_H
#define CFUNCTIONJOB_H

#include "IJob.h"

//! Use a function as a job
/*!
A job class that invokes a function.
*/
class CFunctionJob : public IJob {
public:
	//! run() invokes \c func(arg)
	CFunctionJob(void (*func)(void*), void* arg = NULL);
	virtual ~CFunctionJob();

	// IJob overrides
	virtual void		run();

private:
	void				(*m_func)(void*);
	void*				m_arg;
};

#endif
