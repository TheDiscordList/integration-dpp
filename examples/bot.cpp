// A D++ bot that publishes to TheDiscordList and receives its webhooks.
//
//   DISCORD_TOKEN=... TDL_TOKEN=... TDL_WEBHOOK_SECRET=... ./bot

#include <dpp/dpp.h>
#include <thediscordlist/dpp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string env(const char* name) {
	const char* value = std::getenv(name);
	return value == nullptr ? std::string() : value;
}

}

int main() {
	if (env("DISCORD_TOKEN").empty()) {
		std::cerr << "Set DISCORD_TOKEN.\n";
		return 1;
	}

	dpp::cluster bot(env("DISCORD_TOKEN"));
	bot.on_log(dpp::utility::cout_logger());

	bot.on_slashcommand([](const dpp::slashcommand_t& event) {
		if (event.command.get_command_name() == "ping") {
			event.reply("Pong!");
		}
	});
	bot.on_ready([&bot](const dpp::ready_t&) {
		if (dpp::run_once<struct register_commands>()) {
			bot.global_command_create(dpp::slashcommand("ping", "Check the bot is alive", bot.me.id));
		}
	});

	thediscordlist::options options;
	options.on_disabled = [](thediscordlist::publisher type, int status, const std::string& reason) {
		std::cerr << thediscordlist::to_string(type) << " stopped: " << reason << " (" << status << ")\n";
	};
	thediscordlist::autoposter poster(bot, env("TDL_TOKEN"), options);

#if DPP_VERSION_LONG >= 0x00100100 // dpp::http_server arrived in D++ 10.1.0
	std::unique_ptr<dpp::http_server> webhooks;
	if (std::string secret = env("TDL_WEBHOOK_SECRET"); !secret.empty()) {
		webhooks = std::make_unique<dpp::http_server>(&bot, "0.0.0.0", 8080, [secret](dpp::http_server_request* request) {
			try {
				auto event = thediscordlist::verify_webhook(request->get_request_body(),
					request->get_header(thediscordlist::webhook_signature_header), secret);
				if (event.event == "listing.upvoted" && event.data["user"].is_object()) {
					std::cout << event.data["user"]["username"].get<std::string>() << " upvoted " << event.listing.name << "\n";
				}
				// 2xx for every event, known or not: ten failed deliveries disable the endpoint.
				request->set_status(204);
			} catch (const thediscordlist::webhook_error&) {
				request->set_status(400);
			}
		});
	}
#endif

	bot.start(dpp::st_wait);
}
