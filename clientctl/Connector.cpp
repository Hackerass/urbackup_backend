/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2016 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "Connector.h"
#include "tcpstack.h"
#include "../stringtools.h"
#include "../urbackupcommon/escape.h"
#include "../urbackupcommon/os_functions.h"
#include "../socket_header.h"
#include "json/json.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <iostream>
#include <memory.h>
#include <stdlib.h>
#include <stdexcept>

std::string Connector::pw;
bool Connector::error=false;
const size_t conn_retries=4;
bool Connector::busy=false;
std::string Connector::pwfile="pw.txt";
std::string Connector::pwfile_change="pw_change.txt";
std::string Connector::client="127.0.0.1";
std::string Connector::tokens;

namespace
{
	bool LookupBlocking(std::string pServer, in_addr *dest)
	{
		const char* host=pServer.c_str();
		unsigned int addr = inet_addr(host);
		if (addr != INADDR_NONE)
		{
			dest->s_addr = addr;
		}
		else
		{
			hostent* hp = gethostbyname(host);
			if (hp != 0)
			{
				memcpy(dest, hp->h_addr, hp->h_length);
			}
			else
			{
				return false;
			}
		}
		return true;
	}

	void read_tokens(std::string token_path, std::string& tokens)
	{
            if(os_directory_exists(os_file_prefix(token_path)))
            {
		std::vector<SFile> token_files = getFiles(token_path);

		for(size_t i=0;i<token_files.size();++i)
		{
			if(token_files[i].isdir)
			{
				continue;
			}

			std::string nt = getFile(token_path + os_file_sep() + token_files[i].name);
			if(!nt.empty())
			{
				if(!tokens.empty())
				{
					tokens+=";";
				}
				tokens+=nt;
			}
		}
            }
	}
}


std::string Connector::getResponse(const std::string &cmd, const std::string &args, bool change_command)
{
#ifdef _WIN32
    {
	 WSADATA wsaData = {0};
	int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (rc != 0) {
            wprintf(L"WSAStartup failed: %d\n", rc);
            return "";
        }
    }
#endif


	busy=true;
	error=false;
	std::string pw;
	if(!change_command)
	{
		pw=trim(getFile(pwfile));
	}
	else
	{
		pw=trim(getFile(pwfile_change));
	}

	int type = SOCK_STREAM;
#if !defined(_WIN32) && defined(SOCK_CLOEXEC)
	type |= SOCK_CLOEXEC;
#endif
	SOCKET p=socket(AF_INET, type, 0);

#ifdef __APPLE__
	int optval = 1;
	setsockopt(p, SOL_SOCKET, SO_NOSIGPIPE, (void*)&optval, sizeof(optval));
#endif

	sockaddr_in addr;
	memset(&addr,0,sizeof(sockaddr_in));
	if(!LookupBlocking(client, &addr.sin_addr))
	{
		std::cout << "Error while getting hostname for '" << client << "'" << std::endl;
		return "";
	}
	addr.sin_family=AF_INET;
	addr.sin_port=htons(35623);

        int rc=connect(p, (sockaddr*)&addr, sizeof(sockaddr));

	if(rc==SOCKET_ERROR)
		return "";

	std::string t_args;

	if(!args.empty())
		t_args="&"+args;
	else
		t_args=args;

	CTCPStack tcpstack;
	tcpstack.Send(p, cmd+"#pw="+pw+t_args);

	char *resp=NULL;
	char buffer[1024];
	size_t packetsize;
	while(resp==NULL)
	{
                int rc=recv(p, buffer, 1024, MSG_NOSIGNAL);
		
                if(rc<=0)
		{
			closesocket(p);
			error=true;
			busy=false;
			return "";
		}
		tcpstack.AddData(buffer, rc );
		

		resp=tcpstack.getPacket(&packetsize);
		if(packetsize==0)
		{
			closesocket(p);
			busy=false;
			return "";
		}
	}

	std::string ret;
	ret.resize(packetsize);
	memcpy(&ret[0], resp, packetsize);
	delete resp;

	closesocket(p);

	busy=false;
	return ret;
}

bool Connector::hasError(void)
{
	return error;
}

std::vector<SBackupDir> Connector::getSharedPaths(void)
{
	std::vector<SBackupDir> ret;
	std::string d = getResponse("GET BACKUP DIRS", "", false);

	if (d.empty())
	{
		error = true;
		return ret;
	}

	Json::Value root;
	Json::Reader reader;

	if (!reader.parse(d, root, false))
	{
		return ret;
	}

	try
	{
		Json::Value dirs = root["dirs"];

		for (Json::Value::ArrayIndex i = 0; i<dirs.size(); ++i)
		{
			Json::Value dir = dirs[i];

			std::string virtual_client = dir.get("virtual_client", std::string()).asString();

			SBackupDir rdir =
			{
				dir["path"].asString(),
				dir["name"].asString(),
				dir["id"].asInt(),
				dir["group"].asInt(),
				virtual_client,
				dir["flags"].asString()
			};

			ret.push_back(rdir);
		}
	}
	catch (std::runtime_error&)
	{
	}

	return ret;
}

std::string Connector::escapeParam(const std::string &name)
{
	std::string tmp = greplace("%", "%25", name);
	tmp = greplace("=", "%3D", tmp);
	tmp = greplace("&", "%26", tmp);
	tmp = greplace("$", "%24", tmp);
	return tmp;
}

bool Connector::saveSharedPaths(const std::vector<SBackupDir> &res)
{
	std::string args;
	for (size_t i = 0; i<res.size(); ++i)
	{
		if (i != 0)
			args += "&";

		std::string path = escapeParam(res[i].path);
		std::string name = escapeParam(res[i].name);

		args += "dir_" + convert(i) + "=" + path;
		args += "&dir_" + convert(i) + "_name=" + name;
		args += "&dir_" + convert(i) + "_group=" + convert(res[i].group);

		if (!res[i].virtual_client.empty())
		{
			args += "&dir_" + convert(i) + "_virtual_client=" + escapeParam(res[i].virtual_client);
		}
	}

	std::string d = getResponse("SAVE BACKUP DIRS", args, true);

	if (d != "OK")
		return false;
	else
		return true;
}

SStatus Connector::getStatus(void)
{
	std::string d=getResponse("STATUS","",false);

	std::vector<std::string> toks;
	Tokenize(d, toks, "#");

	SStatus ret;
	ret.pause=false;
	if(toks.size()>0)
		ret.lastbackupdate=toks[0];
	if(toks.size()>1)
		ret.status=toks[1];
	if(toks.size()>2)
		ret.pcdone=toks[2];
	if(toks.size()>3)
	{
		if(toks[3]=="P")
			ret.pause=true;
		else if(toks[3]=="NP")
			ret.pause=false;
	}


	return ret;
}

int Connector::startBackup(bool full)
{
	std::string s;
	if(full)
		s="START BACKUP FULL";
	else
		s="START BACKUP INCR";

	std::string d=getResponse(s,"",false);

	if(d=="RUNNING")
		return 2;
	else if(d=="NO SERVER")
		return 3;
	else if(d!="OK")
		return 0;
	else
		return 1;
}

int Connector::startImage(bool full)
{
	std::string s;
	if(full)
		s="START IMAGE FULL";
	else
		s="START IMAGE INCR";

	std::string d=getResponse(s,"",false);

	if(d=="RUNNING")
		return 2;
	else if(d=="NO SERVER")
		return 3;
	else if(d!="OK")
		return 0;
	else
		return 1;
}

bool Connector::updateSettings(const std::string &ndata, bool& no_perm)
{
	no_perm = false;

	std::string data=ndata;
	escapeClientMessage(data);
	std::string d=getResponse("UPDATE SETTINGS "+data,"", true);

	if (d != "OK" && d != "NOSERVER")
	{
		if (d == "FAILED")
		{
			no_perm = true;
		}

		return false;
	}
	else
	{
		return true;
	}
}

std::vector<SLogEntry> Connector::getLogEntries(void)
{
	std::string d=getResponse("GET LOGPOINTS","", true);
	int lc=linecount(d);
	std::vector<SLogEntry> ret;
	for(int i=0;i<lc;++i)
	{
		std::string l=getline(i, d);
		if(l.empty())continue;
		SLogEntry le;
		le.logid=atoi(getuntil("-", l).c_str() );
		std::string lt=getafter("-", l);
		le.logtime=lt;
		ret.push_back(le);
	}
	return ret;
}

std::vector<SLogLine>  Connector::getLogdata(int logid, int loglevel)
{
	std::string d=getResponse("GET LOGDATA","logid="+convert(logid)+"&loglevel="+convert(loglevel), true);
	std::vector<std::string> lines;
	TokenizeMail(d, lines, "\n");
	std::vector<SLogLine> ret;
	for(size_t i=0;i<lines.size();++i)
	{
		std::string l=lines[i];
		if(l.empty())continue;
		SLogLine ll;
		ll.loglevel=atoi(getuntil("-", l).c_str());
		ll.msg=getafter("-", l);
		ret.push_back(ll);
	}
	return ret;
}

bool Connector::setPause(bool b_pause)
{
	std::string data=b_pause?"true":"false";
	std::string d=getResponse("PAUSE "+data,"", false);

	if(d!="OK")
		return false;
	else
		return true;
}

bool Connector::isBusy(void)
{
	return busy;
}

void Connector::setPWFile(const std::string &pPWFile)
{
	pwfile=pPWFile;
}


void Connector::setPWFileChange( const std::string &pPWFile )
{
	pwfile_change=pPWFile;
}


void Connector::setClient(const std::string &pClient)
{
	client=pClient;
}

std::string Connector::getFileBackupsList(bool& no_server)
{
	no_server=false;

	if(!readTokens())
	{
		return std::string();
	}

	std::string list = getResponse("GET FILE BACKUPS TOKENS", "tokens="+tokens, false);

	if(!list.empty())
	{
		if(list[0]!='0')
		{
			if(list[0]=='1')
			{
				no_server=true;
			}

			return std::string();
		}
		else
		{
			return list.substr(1);
		}
	}
	else
	{
		return std::string();
	}
}

bool Connector::readTokens()
{
	if(!tokens.empty())
	{
		return true;
	}

	read_tokens("tokens", tokens);

#if !defined(_WIN32) && !defined(__APPLE__)
	read_tokens("/var/urbackup/tokens", tokens);
	read_tokens("/usr/local/var/urbackup/tokens", tokens);
#endif

#ifdef __APPLE__
	read_tokens("/usr/var/urbackup/tokens", tokens);
#endif

	return !tokens.empty();
}

std::string Connector::getFileList( const std::string& path, int* backupid, bool& no_server)
{
	no_server=false;

	if(!readTokens())
	{
		return std::string();
	}

	std::string params = "tokens="+tokens;

	if(!path.empty())
	{
		params+="&path="+EscapeParamString(path);
	}

	if(backupid!=NULL)
	{
		params+="&backupid="+convert(*backupid);
	}

	std::string list = getResponse("GET FILE LIST TOKENS",
		params, false);

	if(!list.empty())
	{
		if(list[0]!='0')
		{
			if(list[0]=='1')
			{
				no_server=true;
			}

			return std::string();
		}
		else
		{
			return list.substr(1);
		}
	}
	else
	{
		return std::string();
	}
}

std::string Connector::startRestore( const std::string& path, int backupid,
	const std::vector<SPathMap>& map_paths, bool& no_server, bool clean_other,
	bool ignore_other_fs)
{
	no_server=false;

	if(!readTokens())
	{
		return std::string();
	}

	std::string params = "tokens="+tokens;
	params+="&path="+EscapeParamString(path);
	params+="&backupid="+convert(backupid);

	for (size_t i = 0; i < map_paths.size(); ++i)
	{
		params += "&map_path_source"+convert(i)+"=" + EscapeParamString(map_paths[i].source);
		params += "&map_path_target" + convert(i) + "=" + EscapeParamString(map_paths[i].target);
	}

	params += std::string("&clean_other=") + (clean_other ? "1" : "0");
	params += std::string("&ignore_other_fs=") + (ignore_other_fs ? "1" : "0");

	std::string res = getResponse("DOWNLOAD FILES TOKENS",
		params, false);

	if(!res.empty())
	{
		if(res[0]!='0')
		{
			if(res[0]=='1')
			{
				no_server=true;
			}

			return std::string();
		}
		else
		{
			return res.substr(1);
		}
	}
	else
	{
		return std::string();
	}
}

std::string Connector::getStatusDetailsRaw()
{
	return getResponse("STATUS DETAIL", "", false);
}

SStatusDetails Connector::getStatusDetails()
{
	std::string d = getResponse("STATUS DETAIL", "", false);

	SStatusDetails ret;
	ret.ok = false;

	Json::Value root;
	Json::Reader reader;

	if (!reader.parse(d, root, false))
	{
		return ret;
	}

	try
	{
		ret.last_backup_time = root["last_backup_time"].asInt64();

		Json::Value json_running_processes = root["running_processes"];
		ret.running_processes.resize(json_running_processes.size());
		for (unsigned int i = 0; i<json_running_processes.size(); ++i)
		{
			ret.running_processes[i].action = json_running_processes[i]["action"].asString();
			ret.running_processes[i].percent_done = json_running_processes[i]["percent_done"].asInt();
			ret.running_processes[i].eta_ms = json_running_processes[i]["eta_ms"].asInt64();

			ret.running_processes[i].details = json_running_processes[i].get("details", std::string()).asString();
			ret.running_processes[i].detail_pc = json_running_processes[i].get("detail_pc", -1).asInt();

			ret.running_processes[i].total_bytes = json_running_processes[i].get("total_bytes", -1).asInt64();
			ret.running_processes[i].done_bytes = json_running_processes[i].get("done_bytes", 0).asInt64();

			ret.running_processes[i].process_id = json_running_processes[i].get("process_id", 0).asInt64();
			ret.running_processes[i].server_status_id = json_running_processes[i].get("server_status_id", 0).asInt64();

			ret.running_processes[i].speed_bpms = json_running_processes[i].get("speed_bpms", 0).asDouble();
		}

		Json::Value json_finished_processes = root["finished_processes"];
		ret.finished_processes.resize(json_finished_processes.size());
		for (unsigned int i = 0; i < json_finished_processes.size(); ++i)
		{
			ret.finished_processes[i].id = json_finished_processes[i]["process_id"].asInt64();
			ret.finished_processes[i].success = json_finished_processes[i]["success"].asBool();
		}

		std::vector<SUrBackupServer> servers;
		Json::Value json_servers = root["servers"];
		servers.resize(json_servers.size());
		for (unsigned int i = 0; i<json_servers.size(); ++i)
		{
			servers[i].internet_connection = json_servers[i]["internet_connection"].asBool();
			servers[i].name = json_servers[i]["name"].asString();
		}
		ret.servers = servers;
		ret.time_since_last_lan_connection = root["time_since_last_lan_connection"].asInt();
		ret.internet_connected = root["internet_connected"].asBool();
		ret.internet_status = root["internet_status"].asString();

		ret.capability_bits = root["capability_bits"].asInt();

		ret.ok = true;

		return ret;
	}
	catch (std::runtime_error&)
	{
		return ret;
	}
}

std::string Connector::resetKeep(const std::string & virtualClient, const std::string & folderName, int tgroup)
{
	std::string params;

	if (!virtualClient.empty())
	{
		params += "virtual_client=" + EscapeParamString(virtualClient);
	}

	if (!folderName.empty())
	{
		if (!params.empty()) params += "&";

		params += "folder_name=" + EscapeParamString(folderName);
	}

	if (!params.empty()) params += "&";

	params += "tgroup=" + convert(tgroup);

	std::string d = getResponse("RESET KEEP", params, true);

	return d;
}
