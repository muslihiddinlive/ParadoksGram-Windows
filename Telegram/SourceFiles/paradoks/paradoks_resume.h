/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Paradoks::Resume {

// A failed (or interrupted by closing) download of a message document keeps
// its downloaded part (see mtpFileLoader) and is remembered here, so that it
// is continued automatically after the next start of the application.
void Remember(
	UserId user,
	FullMsgId origin,
	uint64 documentId,
	const QString &path);
void Forget(UserId user, uint64 documentId);

// Called for every session: shortly after the start continues the
// remembered downloads of this session.
void StartForSession(not_null<Main::Session*> session);

} // namespace Paradoks::Resume
