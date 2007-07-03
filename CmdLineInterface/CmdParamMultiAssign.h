#pragma once
#ifndef _CMD_PARAM_MULTI_ASSIGN_H_
#define _CMD_PARAM_MULTI_ASSIGN_H_

#include "CmdParam.h"
#include <list>

class CmdParamMultiAssign : public CmdParam {
	std::list<std::pair<std::string, std::string> > m_strings;
	std::list<std::pair<std::string, std::string> >::iterator m_it;
public:
	CmdParamMultiAssign(const char* paramName, const char* description, const char* argumentDesciption, int flags);

	int parse(const char* str, char* errorMsg, int buffsize);

	const char* getValue1();
	const char* getValue2();
	void next();
	bool hasNext() const;
};

#endif
