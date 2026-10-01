#include <thediscordlist/webhooks.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace thediscordlist {

namespace {

std::string_view trim(std::string_view text) {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
		text.remove_prefix(1);
	}
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
		text.remove_suffix(1);
	}
	return text;
}

std::optional<double> parse_finite(std::string_view text) {
	if (text.empty()) {
		return std::nullopt;
	}
	std::string copy(text);
	char* end = nullptr;
	double value = std::strtod(copy.c_str(), &end);
	if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
		return std::nullopt;
	}
	return value;
}

std::string hmac_sha256_hex(std::string_view secret, const std::string& message) {
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	HMAC(EVP_sha256(), secret.empty() ? "" : secret.data(), static_cast<int>(secret.size()),
		reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &length);

	static constexpr char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(length * 2);
	for (unsigned int i = 0; i < length; ++i) {
		out += hex[digest[i] >> 4];
		out += hex[digest[i] & 0x0F];
	}
	return out;
}

std::string string_field(const nlohmann::json& object, const char* key) {
	auto found = object.find(key);
	return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

}

const char* to_string(webhook_error_code code) noexcept {
	switch (code) {
		case webhook_error_code::missing_signature: return "MISSING_SIGNATURE";
		case webhook_error_code::malformed_signature: return "MALFORMED_SIGNATURE";
		case webhook_error_code::stale_timestamp: return "STALE_TIMESTAMP";
		case webhook_error_code::invalid_signature: return "INVALID_SIGNATURE";
		case webhook_error_code::malformed_body: return "MALFORMED_BODY";
	}
	return "UNKNOWN";
}

webhook_error::webhook_error(webhook_error_code code, const std::string& message)
	: std::runtime_error(message), code_(code) {
}

webhook_event verify_webhook(std::string_view raw_body, std::string_view signature_header, std::string_view secret,
	std::chrono::seconds tolerance, std::optional<std::time_t> now) {
	if (trim(signature_header).empty()) {
		throw webhook_error(webhook_error_code::missing_signature, "Missing X-TDL-Signature header.");
	}

	// Split each part on its first '=' only: a value may legitimately contain '='.
	std::optional<std::string_view> timestamp;
	std::optional<std::string_view> signature;
	std::string_view rest = signature_header;
	while (!rest.empty()) {
		auto comma = rest.find(',');
		auto part = rest.substr(0, comma);
		rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);

		auto eq = part.find('=');
		if (eq == std::string_view::npos) {
			continue;
		}
		auto key = trim(part.substr(0, eq));
		auto value = trim(part.substr(eq + 1));
		if (key == "t" && !timestamp) {
			timestamp = value;
		} else if (key == "v1" && !signature && !value.empty()) {
			signature = value;
		}
	}

	auto t = timestamp ? parse_finite(*timestamp) : std::nullopt;
	if (!t || !signature) {
		throw webhook_error(webhook_error_code::malformed_signature, "Malformed X-TDL-Signature header.");
	}

	// Two-sided, and against t rather than created_at: created_at is stamped at enqueue
	// time and can be over an hour old on a retry.
	double current = static_cast<double>(now ? *now : std::time(nullptr));
	if (std::fabs(current - *t) > static_cast<double>(tolerance.count())) {
		throw webhook_error(webhook_error_code::stale_timestamp, "Webhook timestamp outside the tolerance window.");
	}

	std::string message;
	message.reserve(timestamp->size() + 1 + raw_body.size());
	message.append(*timestamp).append(1, '.').append(raw_body);
	auto expected = hmac_sha256_hex(secret, message);

	std::string provided(*signature);
	for (auto& c : provided) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (provided.size() != expected.size() || CRYPTO_memcmp(provided.data(), expected.data(), expected.size()) != 0) {
		throw webhook_error(webhook_error_code::invalid_signature, "Webhook signature did not match.");
	}

	auto payload = nlohmann::json::parse(raw_body.begin(), raw_body.end(), nullptr, false);
	if (payload.is_discarded() || !payload.is_object()) {
		throw webhook_error(webhook_error_code::malformed_body, "Webhook body was not a JSON object.");
	}

	webhook_event event;
	event.id = string_field(payload, "id");
	event.event = string_field(payload, "event");
	event.created_at = string_field(payload, "created_at");
	auto listing = payload.find("listing");
	if (listing != payload.end() && listing->is_object()) {
		event.listing = {string_field(*listing, "id"), string_field(*listing, "type"),
			string_field(*listing, "name"), string_field(*listing, "url")};
	}
	auto data = payload.find("data");
	event.data = data != payload.end() ? *data : nlohmann::json::object();
	return event;
}

}
