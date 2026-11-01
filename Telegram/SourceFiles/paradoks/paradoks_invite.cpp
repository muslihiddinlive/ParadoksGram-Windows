/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_invite.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/share_box.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

namespace Paradoks::Invite {
namespace {

[[nodiscard]] QString InviteText() {
	return u"Salom, men ParadoksGramdan foydalanyapman. Menga qo'shiling! "
		"Uni bu yerdan yuklab oling: https://ParadoksGram.pages.dev"_q;
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	const auto holder = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	const auto toast = base::make_weak(controller);
	const auto session = &controller->session();
	auto box = Box<ShareBox>(ShareBox::Descriptor{
		.session = session,
		.submitCallback = [=](
				std::vector<not_null<Data::Thread*>> &&threads,
				Fn<bool()>,
				TextWithTags &&,
				Api::SendOptions,
				Data::ForwardOptions) {
			for (const auto &thread : threads) {
				auto message = Api::MessageToSend(Api::SendAction(thread));
				message.textWithTags = TextWithTags{ InviteText() };
				session->api().sendMessage(std::move(message));
			}
			if (const auto strong = holder->get()) {
				strong->closeBox();
			}
			if (const auto strong = toast.get()) {
				strong->showToast(u"Taklif yuborildi: %1 ta chat"_q.arg(
					threads.size()));
			}
		},
		.filterCallback = [](not_null<Data::Thread*> thread) {
			return Data::CanSendAnything(thread->peer());
		},
		.titleOverride = rpl::single(u"Do'stlarni taklif qilish"_q),
	});
	*holder = base::make_weak(box.data());
	controller->show(std::move(box));
}

int OnlineContacts(not_null<Main::Session*> session) {
	auto result = 0;
	const auto now = base::unixtime::now();
	for (const auto &row : session->data().contactsList()->all()) {
		const auto history = row->history();
		const auto user = history ? history->peer->asUser() : nullptr;
		if (user && !user->isSelf() && Data::OnlineTextActive(user, now)) {
			++result;
		}
	}
	return result;
}

void OpenWallet(not_null<Window::SessionController*> controller) {
	controller->window().hideSettingsAndLayer();
	controller->showPeerByLink(Window::PeerByLinkInfo{
		.usernameOrId = u"wallet"_q,
		.attachBotToggleCommand = QString(),
		.attachBotMainOpen = true,
	});
}

} // namespace Paradoks::Invite
