#include "CmdParamString.h"

CmdParamString::CmdParamString(const char* paramName, const char* description, const char* argumentDesciption, int flags, const char* defaultValue) :
CmdParam(paramName, description, argumentDesciption, flags | PARAM_TAKES_ARGUMENT) {
	if(defaultValue != NULL) {
		m_strings.push_back(defaultValue);
	}
	m_it = m_strings.begin();
}

int CmdParamString::parse(const char* str, char* errorMsg, int buffsize) {
	if(getFlags() & PARAM_FORBID_MULTIPLE_DEFINITIONS)
		m_strings.clear();
	m_strings.push_back(str);
	m_it = m_strings.begin();
	return PARSE_OK;
}

const char* CmdParamString::getValue() {
	return hasNext() ? m_it->c_str() : NULL;
}

void CmdParamString::next() {
	m_it++;
}

bool CmdParamString::hasNext() const {
	return m_it != m_strings.end();
}

std::list<std::string> CmdParamString::getList() {
	return m_strings;
}
