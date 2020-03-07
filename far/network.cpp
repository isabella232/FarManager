﻿/*
network.cpp

misc network functions
*/
/*
Copyright © 2009 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Self:
#include "network.hpp"

// Internal:
#include "lang.hpp"
#include "message.hpp"
#include "stddlg.hpp"
#include "drivemix.hpp"
#include "flink.hpp"
#include "cddrv.hpp"
#include "pathmix.hpp"
#include "strmix.hpp"
#include "exception.hpp"

// Platform:
#include "platform.fs.hpp"
#include "platform.reg.hpp"

// Common:
#include "common.hpp"
#include "common/scope_exit.hpp"

// External:

//----------------------------------------------------------------------------

static string GetStoredUserName(wchar_t Drive)
{
	//Тут может быть надо заюзать WNetGetUser
	string UserName;
	// BUGBUG check result
	(void)os::reg::key::current_user.get(concat(L"Network\\"sv, Drive), L"UserName"sv, UserName);
	return UserName;
}

os::fs::drives_set GetSavedNetworkDrives()
{
	HANDLE hEnum;
	if (WNetOpenEnum(RESOURCE_REMEMBERED, RESOURCETYPE_DISK, 0, nullptr, &hEnum) != NO_ERROR)
		return 0;

	SCOPE_EXIT{ WNetCloseEnum(hEnum); };

	os::fs::drives_set Drives;

	block_ptr<NETRESOURCE> Buffer(16 * 1024);

	for (;;)
	{
		DWORD Count = -1;
		auto BufferSize = static_cast<DWORD>(Buffer.size());
		const auto Result = WNetEnumResource(hEnum, &Count, Buffer.data(), &BufferSize);

		if (Result == ERROR_MORE_DATA)
		{
			Buffer.reset(BufferSize);
			continue;
		}

		if (Result != NO_ERROR || !Count)
			break;

		for (const auto& i: span(Buffer.data(), Count))
		{
			const auto Name = i.lpLocalName;
			if (os::fs::is_standard_drive_letter(Name[0]) && Name[1] == L':')
			{
				Drives.set(os::fs::get_drive_number(Name[0]));
			}
		}
	}

	return Drives;
}

bool ConnectToNetworkResource(const string& NewDir)
{
	string LocalName, RemoteName;

	const auto IsDrive = ParsePath(NewDir) == root_type::drive_letter;
	if (IsDrive)
	{
		LocalName = NewDir.substr(0, 2);
		// TODO: check result
		DriveLocalToRemoteName(DRIVE_REMOTE_NOT_CONNECTED, NewDir[0], RemoteName);
	}
	else
	{
		LocalName = NewDir;
		RemoteName = NewDir;
	}

	auto UserName = IsDrive? GetStoredUserName(NewDir[0]) : L""s;

	NETRESOURCE netResource{};
	netResource.dwType = RESOURCETYPE_DISK;
	netResource.lpLocalName = IsDrive? UNSAFE_CSTR(LocalName) : nullptr;
	netResource.lpRemoteName = UNSAFE_CSTR(RemoteName);
	netResource.lpProvider = nullptr;

	if (const auto Result = WNetAddConnection2(&netResource, nullptr, EmptyToNull(UserName), 0); Result == NO_ERROR ||
		(Result == ERROR_SESSION_CREDENTIAL_CONFLICT && WNetAddConnection2(&netResource, nullptr, nullptr, 0) == NO_ERROR))
	{
		return true;
	}

	string Password;

	for (;;)
	{
		if (!GetNameAndPassword(RemoteName, UserName, Password, {}, GNP_USELAST))
			return false;

		if (const auto Result = WNetAddConnection2(&netResource, Password.c_str(), EmptyToNull(UserName), 0); Result == NO_ERROR)
			return true;
		else if (Result != ERROR_ACCESS_DENIED && Result != ERROR_INVALID_PASSWORD && Result != ERROR_LOGON_FAILURE)
		{
			Message(MSG_WARNING, error_state::fetch(),
				msg(lng::MError),
				{
					NewDir
				},
				{ lng::MOk });
			return false;
		}
	}
}

string ExtractComputerName(const string_view CurDir, string* const strTail)
{
	if (strTail)
		strTail->clear();

	const auto IsSuppportedPathType = [](root_type const Type)
	{
		return Type == root_type::remote || Type == root_type::unc_remote;
	};

	string strNetDir;

	if (IsSuppportedPathType(ParsePath(CurDir)))
	{
		strNetDir = CurDir;
	}
	else
	{
		os::WNetGetConnection(CurDir.substr(0, 2), strNetDir);
	}

	if (strNetDir.empty())
		return {};

	const auto NetDirPathType = ParsePath(strNetDir);
	if (!IsSuppportedPathType(NetDirPathType))
		return {};

	auto Result = strNetDir.substr(NetDirPathType == root_type::remote? 2 : 8);
	const auto pos = FindSlash(Result);
	if (pos == string::npos)
		return {};

	if (strTail)
		strTail->assign(Result, pos + 1, string::npos); // gcc 7.3-8.1 bug: npos required. TODO: Remove after we move to 8.2 or later)

	Result.resize(pos);

	return Result;
}

bool DriveLocalToRemoteName(int DriveType, wchar_t Letter, string &strDest)
{
	const auto LocalName = os::fs::get_drive(Letter);

	if (DriveType == DRIVE_UNKNOWN)
	{
		DriveType = FAR_GetDriveType(LocalName);
	}

	string strRemoteName;

	if (IsDriveTypeRemote(DriveType) && os::WNetGetConnection(LocalName, strRemoteName))
	{
		strDest = strRemoteName;
		return true;
	}

	if (GetSubstName(DriveType, LocalName, strRemoteName))
	{
		strDest = strRemoteName;
		return true;
	}

	return false;
}
