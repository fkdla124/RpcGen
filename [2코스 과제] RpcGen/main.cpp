// RpcGen.cpp
// Usage: RpcGen <Protocol.rpc> [outputDir]
// Generates:
//   Common.h (MessageType enum, shared)
//   RPC_ServerProxy.h / .cpp   <- SC_ messages, sent by the server
//   RPC_ServerStub.h  / .cpp   <- CS_ messages, received by the server
//   RPC_ClientProxy.h / .cpp   <- CS_ messages, sent by the client
//   RPC_ClientStub.h  / .cpp   <- SC_ messages, received by the client

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Param
{
	std::string type;
	std::string name;
};

struct Message
{
	std::string fullName;	// SC_MoveStart
	std::string funcName;	// MoveStart
	bool toClient = false;	// true: SC_, false: CS_
	int number = 0;
	bool explicitNumber = false;
	std::vector<Param> params;
	int lineNo = 0;
};

// ------------------------------------------------------------
// parsing
// ------------------------------------------------------------
static std::string Trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

static bool IsIdentifier(const std::string& s)
{
	if (s.empty() || std::isdigit((unsigned char)s[0]))
		return false;
	for (char c : s)
		if (!std::isalnum((unsigned char)c) && c != '_')
			return false;
	return true;
}

[[noreturn]] static void Fail(int lineNo, const std::string& msg)
{
	throw std::runtime_error("line " + std::to_string(lineNo) + ": " + msg);
}

static Message ParseLine(const std::string& line, int lineNo, int& nextNumber)
{
	Message m;
	m.lineNo = lineNo;

	size_t open = line.find('(');
	size_t close = (open == std::string::npos) ? std::string::npos : line.find(')', open);
	size_t semi = (close == std::string::npos) ? std::string::npos : line.find(';', close);
	if (semi == std::string::npos)
		Fail(lineNo, "expected format: NAME(type name, ...); [number]");

	// name
	m.fullName = Trim(line.substr(0, open));
	if (!IsIdentifier(m.fullName))
		Fail(lineNo, "invalid message name '" + m.fullName + "'");

	if (m.fullName.rfind("SC_", 0) == 0)
		m.toClient = true;
	else if (m.fullName.rfind("CS_", 0) == 0)
		m.toClient = false;
	else
		Fail(lineNo, "message name must start with SC_ or CS_ : " + m.fullName);

	m.funcName = m.fullName.substr(3);
	if (!IsIdentifier(m.funcName))
		Fail(lineNo, "invalid function name '" + m.funcName + "'");

	// params
	std::string paramStr = Trim(line.substr(open + 1, close - open - 1));
	if (!paramStr.empty())
	{
		std::stringstream ss(paramStr);
		std::string token;
		while (std::getline(ss, token, ','))
		{
			token = Trim(token);
			size_t sp = token.find_last_of(" \t");
			if (sp == std::string::npos)
				Fail(lineNo, "parameter needs 'type name' : '" + token + "'");

			Param p;
			p.type = Trim(token.substr(0, sp));
			p.name = Trim(token.substr(sp + 1));
			if (p.type.empty() || !IsIdentifier(p.name))
				Fail(lineNo, "invalid parameter '" + token + "'");
			m.params.push_back(p);
		}
	}

	// optional message number after ';'
	std::string tail = Trim(line.substr(semi + 1));
	size_t hash = tail.find('#');
	if (hash != std::string::npos)
		tail = Trim(tail.substr(0, hash));

	if (!tail.empty())
	{
		for (char c : tail)
			if (!std::isdigit((unsigned char)c))
				Fail(lineNo, "invalid message number '" + tail + "'");
		nextNumber = std::stoi(tail);
		m.explicitNumber = true;
	}

	if (nextNumber < 0 || nextNumber > 255)
		Fail(lineNo, "message number out of range (0~255) : " + std::to_string(nextNumber));

	m.number = nextNumber++;
	return m;
}

static std::vector<Message> ParseFile(const fs::path& path)
{
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("cannot open " + path.string());

	std::vector<Message> list;
	std::set<std::string> names;
	std::set<int> numbers;

	int nextNumber = 0;
	int lineNo = 0;
	std::string line;

	while (std::getline(in, line))
	{
		++lineNo;

		// ignore: empty line / first char is '#', space, tab, newline
		if (line.empty())
			continue;
		char c = line[0];
		if (c == '#' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;

		Message m = ParseLine(line, lineNo, nextNumber);

		if (!names.insert(m.fullName).second)
			Fail(lineNo, "duplicate message name " + m.fullName);
		if (!numbers.insert(m.number).second)
			Fail(lineNo, "duplicate message number " + std::to_string(m.number) + " (" + m.fullName + ")");

		list.push_back(m);
	}
	return list;
}

// ------------------------------------------------------------
// code generation helpers
// ------------------------------------------------------------
// Function the generated client proxy calls to send a packet: Func(Session*, MsgBuf&)
static const char* kClientSendFunc = "SendUnicast";

static const char* kBanner =
"// ------------------------------------------------------------\n"
"// AUTO-GENERATED by RpcGen from Protocol.rpc. DO NOT EDIT.\n"
"// ------------------------------------------------------------\n";

static std::string ParamDecl(const std::vector<Param>& params)
{
	std::string s;
	for (const Param& p : params)
		s += ", " + p.type + " " + p.name;
	return s;
}

static std::string LenExpr(const std::vector<Param>& params)
{
	if (params.empty())
		return "0";

	std::string s;
	for (size_t i = 0; i < params.size(); ++i)
	{
		if (i) s += " + ";
		s += "sizeof(" + params[i].type + ")";
	}
	return s;
}

static std::string Chain(const char* op, const std::vector<Param>& params)
{
	std::string s;
	for (const Param& p : params)
		s += std::string(" ") + op + " " + p.name;
	return s;
}

// ------------------------------------------------------------
// generators
// ------------------------------------------------------------
static void GenCommon(std::ostream& o, const std::vector<Message>& msgs)
{
	o << kBanner << "#pragma once\n\n";
	o << "enum class MessageType\n{\n";
	for (size_t i = 0; i < msgs.size(); ++i)
	{
		const Message& m = msgs[i];
		if (i > 0 && m.explicitNumber)
			o << "\n";
		o << "\t" << m.fullName;
		if (m.explicitNumber)
			o << " = " << m.number;
		o << ",\n";
	}
	o << "};\n";
}

static void GenProxyH(std::ostream& o, const std::vector<Message>& msgs)
{
	o << kBanner << "#pragma once\n\n";
	o << "#include \"Session.h\"\n\n";
	o << "enum class SendType\n{\n\tUnicast = 0,\n\tBroadcast = 1\n};\n\n";

	o << "namespace RPC\n{\n";
	for (const Message& m : msgs)
		if (m.toClient)
			o << "\tvoid " << m.funcName << "(SendType sendType, Session* session" << ParamDecl(m.params) << ");\n";
	o << "}\n";
}

static std::string ProxyFuncText(const Message& m, const char* sessionName, const char* sendFunc)
{
	std::ostringstream o;
	o << "void " << m.funcName << "(Session* " << sessionName << ParamDecl(m.params) << ")\n{\n";
	o << "\tMessageHeader header;\n";
	o << "\theader.packetCode = (char)0x89;\n";
	o << "\theader.type = (char)MessageType::" << m.fullName << ";\n";
	o << "\theader.len = " << LenExpr(m.params) << ";\n\n";
	o << "\tMsgBuf msgBuf;\n";
	o << "\tmsgBuf << header.packetCode << header.len << header.type;\n";
	if (!m.params.empty())
		o << "\tmsgBuf" << Chain("<<", m.params) << ";\n";
	o << "\n";
	o << "\t" << sendFunc << "(" << sessionName << ", msgBuf);\n";
	o << "}\n";
	return o.str();
}

static std::string ProxyBodies(const std::vector<Message>& msgs, bool toClient, const char* sessionName, const char* sendFunc)
{
	std::string out;
	bool first = true;
	for (const Message& m : msgs)
	{
		if (m.toClient != toClient)
			continue;
		if (!first)
			out += "\n";
		first = false;
		out += ProxyFuncText(m, sessionName, sendFunc);
	}
	return out;
}

static std::string Indent(const std::string& text, int tabs)
{
	std::istringstream in(text);
	std::string line, out;
	const std::string pad(tabs, '\t');
	while (std::getline(in, line))
		out += line.empty() ? "\n" : pad + line + "\n";
	return out;
}

static std::string ServerProxyFuncText(const Message& m)
{
	std::ostringstream o;
	o << "void " << m.funcName << "(SendType sendType, Session* session" << ParamDecl(m.params) << ")\n{\n";
	o << "\tMessageHeader header;\n";
	o << "\theader.packetCode = (char)0x89;\n";
	o << "\theader.type = (char)MessageType::" << m.fullName << ";\n";
	o << "\theader.len = " << LenExpr(m.params) << ";\n\n";
	o << "\tMsgBuf msgBuf;\n";
	o << "\tmsgBuf << header.packetCode << header.len << header.type;\n";
	if (!m.params.empty())
		o << "\tmsgBuf" << Chain("<<", m.params) << ";\n";
	o << "\n";
	o << "\tif (sendType == SendType::Unicast)\n\t\tSendUnicast(session, msgBuf);\n";
	o << "\telse if (sendType == SendType::Broadcast)\n\t\tSendBroadcast(session, msgBuf);\n";
	o << "}\n";
	return o.str();
}

static void GenProxyCpp(std::ostream& o, const std::vector<Message>& msgs)
{
	o << kBanner;
	o << "#include \"RPC_ServerProxy.h\"\n"
		"#include \"Session.h\"\n"
		"#include \"SendPacket.h\"\n"
		"#include \"Protocol.h\"\n"
		"#include \"MsgBuf.h\"\n"
		"#include \"Common.h\"\n";

	std::string body;
	bool first = true;
	for (const Message& m : msgs)
	{
		if (!m.toClient)
			continue;
		if (!first)
			body += "\n";
		first = false;
		body += ServerProxyFuncText(m);
	}

	o << "\nnamespace RPC\n{\n";
	o << Indent(body, 1);
	o << "}\n";
}

static void GenClientProxyH(std::ostream& o, const std::vector<Message>& msgs)
{
	o << kBanner << "#pragma once\n\n";
	o << "#include \"Session.h\"\n\n";
	o << "namespace RPC\n{\n";
	for (const Message& m : msgs)
		if (!m.toClient)
			o << "\tvoid " << m.funcName << "(Session* session" << ParamDecl(m.params) << ");\n";
	o << "}\n";
}

static void GenClientProxyCpp(std::ostream& o, const std::vector<Message>& msgs)
{
	o << kBanner;
	o << "#include \"RPC_ClientProxy.h\"\n"
		"#include \"Session.h\"\n"
		"#include \"SendPacket.h\"\n"
		"#include \"Protocol.h\"\n"
		"#include \"MsgBuf.h\"\n"
		"#include \"Common.h\"\n";

	o << "\nnamespace RPC\n{\n";
	o << Indent(ProxyBodies(msgs, false, "session", kClientSendFunc), 1);
	o << "}\n";
}

// handleToClient == false : server stub (receives CS_ messages)
// handleToClient == true  : client stub (receives SC_ messages)
static void GenStubH(std::ostream& o, const std::vector<Message>& msgs, bool handleToClient)
{
	o << kBanner << "#pragma once\n\n";
	o << "#include \"Session.h\"\n"
		"#include \"Protocol.h\"\n"
		"#include \"MsgBuf.h\"\n"
		"#include \"Common.h\"\n\n";

	o << "class StubInterface\n{\npublic:\n"
		"\tvirtual ~StubInterface() = default;\n"
		"\tvirtual bool MessageProc(Session* session, MessageHeader& header, MsgBuf& msg) = 0;\n"
		"};\n\n";

	o << "class Stub : public StubInterface\n{\npublic:\n";
	o << "\tbool MessageProc(Session* session, MessageHeader& header, MsgBuf& msg) override;\n\n";
	for (const Message& m : msgs)
		if (m.toClient == handleToClient)
			o << "\tvirtual bool " << m.funcName << "(Session* session" << ParamDecl(m.params) << ") = 0;\n";
	o << "};\n";
}

static void GenStubCpp(std::ostream& o, const std::vector<Message>& msgs, bool handleToClient, const char* headerName)
{
	o << kBanner;
	o << "#include \"" << headerName << "\"\n\n";
	o << "bool Stub::MessageProc(Session* session, MessageHeader& header, MsgBuf& msg)\n{\n";
	o << "\tswitch ((MessageType)header.type)\n\t{\n";

	for (const Message& m : msgs)
	{
		if (m.toClient != handleToClient)
			continue;

		o << "\t\tcase MessageType::" << m.fullName << ":\n\t\t{\n";
		for (const Param& p : m.params)
			o << "\t\t\t" << p.type << " " << p.name << ";\n";
		if (!m.params.empty())
			o << "\t\t\tmsg" << Chain(">>", m.params) << ";\n";

		o << "\t\t\treturn this->" << m.funcName << "(session";
		for (const Param& p : m.params)
			o << ", " << p.name;
		o << ");\n\t\t}\n";
	}

	o << "\t\tdefault:\n\t\t{\n\t\t\treturn false;\n\t\t}\n";
	o << "\t}\n}\n";
}

template <typename Fn>
static void WriteFile(const fs::path& path, Fn fn)
{
	std::ofstream out(path, std::ios::trunc);
	if (!out)
		throw std::runtime_error("cannot write " + path.string());
	fn(out);
	std::cout << "generated: " << path.string() << "\n";
}

int main(int argc, char** argv)
{
	// No arguments (e.g. double-click): use ./Protocol.rpc and write into the current folder.
	const bool noArgs = (argc < 2);
	int result = 0;

	try
	{
		fs::path input = noArgs ? fs::path("Protocol.rpc") : fs::path(argv[1]);
		fs::path outDir = (argc >= 3) ? fs::path(argv[2]) : fs::path(".");

		if (noArgs)
			std::cout << "input: " << fs::absolute(input).string() << "\n";

		fs::create_directories(outDir);

		std::vector<Message> msgs = ParseFile(input);

		WriteFile(outDir / "Common.h", [&](std::ostream& o) { GenCommon(o, msgs); });
		WriteFile(outDir / "RPC_ServerProxy.h", [&](std::ostream& o) { GenProxyH(o, msgs); });
		WriteFile(outDir / "RPC_ServerProxy.cpp", [&](std::ostream& o) { GenProxyCpp(o, msgs); });
		WriteFile(outDir / "RPC_ServerStub.h", [&](std::ostream& o) { GenStubH(o, msgs, false); });
		WriteFile(outDir / "RPC_ServerStub.cpp", [&](std::ostream& o) { GenStubCpp(o, msgs, false, "RPC_ServerStub.h"); });

		WriteFile(outDir / "RPC_ClientProxy.h", [&](std::ostream& o) { GenClientProxyH(o, msgs); });
		WriteFile(outDir / "RPC_ClientProxy.cpp", [&](std::ostream& o) { GenClientProxyCpp(o, msgs); });
		WriteFile(outDir / "RPC_ClientStub.h", [&](std::ostream& o) { GenStubH(o, msgs, true); });
		WriteFile(outDir / "RPC_ClientStub.cpp", [&](std::ostream& o) { GenStubCpp(o, msgs, true, "RPC_ClientStub.h"); });
	}
	catch (const std::exception& e)
	{
		std::cerr << "error: " << e.what() << "\n";
		result = 1;
	}

	#ifdef _WIN32
	// keep the console window open when launched by double-click
	if (noArgs)
		std::system("pause");
	#endif
	return result;
}
