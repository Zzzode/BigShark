#include <third_party/yyjson/yyjson.h>

#include <algorithm>
#include <bs/decision.hpp>
#include <bs/v0_protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace bs::v0 {
namespace {

std::string getString(yyjson_val* value) {
  const char* text = yyjson_is_str(value) ? yyjson_get_str(value) : nullptr;
  return text ? text : "";
}

double getNumber(yyjson_val* value, double fallback = 0) {
  return yyjson_is_num(value) ? yyjson_get_num(value) : fallback;
}

long long getInteger(yyjson_val* value, long long fallback = 0) {
  return yyjson_is_num(value) ? static_cast<long long>(yyjson_get_num(value)) : fallback;
}

yyjson_val* getObjectValue(yyjson_val* object, const char* key) {
  return yyjson_is_obj(object) ? yyjson_obj_get(object, key) : nullptr;
}

}  // namespace

Ctx parseRequest(const std::string& json) {
  Ctx context;
  yyjson_doc* document = yyjson_read(json.c_str(), json.size(), 0);
  if (!document)
    return context;
  yyjson_val* root = yyjson_doc_get_root(document);
  auto value = [&](const char* key) { return getObjectValue(root, key); };
  auto stringValue = [&](const char* key) { return getString(value(key)); };

  context.handId = stringValue("handId");
  context.revision = getInteger(value("revision"));
  context.seed = static_cast<uint64_t>(getInteger(value("seed")));
  context.style = stringValue("style");
  if (context.style.empty())
    context.style = "tag";
  context.street = stringValue("street");
  if (context.street.empty())
    context.street = "preflop";

  if (yyjson_val* blinds = value("blinds"); blinds && yyjson_arr_size(blinds) == 2) {
    context.sb = static_cast<int>(getInteger(yyjson_arr_get(blinds, 0)));
    context.bb = static_cast<int>(getInteger(yyjson_arr_get(blinds, 1)));
  }
  context.position = stringValue("position");
  if (context.position.empty())
    context.position = "BTN";
  context.playersInHand = std::max(2, static_cast<int>(getInteger(value("playersInHand"), 2)));
  context.effectiveStackBb = getNumber(value("effectiveStackBb"), 100);
  if (context.effectiveStackBb <= 0)
    context.effectiveStackBb = 100;

  if (yyjson_val* hole = value("hole"))
    for (size_t index = 0, size = yyjson_arr_size(hole); index < size; ++index)
      context.hole.push_back(getString(yyjson_arr_get(hole, index)));
  if (yyjson_val* board = value("board"))
    for (size_t index = 0, size = yyjson_arr_size(board); index < size; ++index)
      context.board.push_back(getString(yyjson_arr_get(board, index)));

  context.pot = static_cast<int>(getInteger(value("pot")));
  context.raises = static_cast<int>(getInteger(value("raises")));
  context.riverGtoOn = yyjson_get_bool(value("riverGtoOn"));
  context.riverLine = stringValue("riverLine");
  context.flopLine = stringValue("flopLine");
  context.turnLine = stringValue("turnLine");
  if (yyjson_val* betFraction = value("riverBetFrac"))
    context.riverBetFrac = getNumber(betFraction, context.riverBetFrac);
  if (yyjson_val* raiseFraction = value("riverRaiseFrac"))
    context.riverRaiseFrac = getNumber(raiseFraction, context.riverRaiseFrac);
  context.openerPosition = stringValue("openerPosition");
  if (yyjson_val* heroWasRaiser = value("heroWasRaiser"))
    context.heroWasRaiser = yyjson_get_bool(heroWasRaiser);
  if (yyjson_val* heroWasAggressor = value("heroPreflopAggressor"))
    context.heroPreflopAggressor = yyjson_get_bool(heroWasAggressor);
  context.limpers = static_cast<int>(getInteger(value("limpers")));
  if (yyjson_val* percentages = value("opponentPcts"))
    for (size_t index = 0, size = yyjson_arr_size(percentages); index < size; ++index)
      context.opponentPcts.push_back(getNumber(yyjson_arr_get(percentages, index)));

  if (yyjson_val* legal = value("legal")) {
    if (yyjson_val* actions = getObjectValue(legal, "actions"))
      for (size_t index = 0, size = yyjson_arr_size(actions); index < size; ++index)
        context.legal.actions.push_back(getString(yyjson_arr_get(actions, index)));
    context.legal.call = static_cast<int>(getInteger(getObjectValue(legal, "call")));
    context.legal.potOdds = getNumber(getObjectValue(legal, "potOdds"));
    if (yyjson_val* raiseTo = getObjectValue(legal, "raiseTo")) {
      context.legal.raiseMin = static_cast<int>(getInteger(getObjectValue(raiseTo, "min")));
      context.legal.raiseMax = static_cast<int>(getInteger(getObjectValue(raiseTo, "max")));
    }
  }

  yyjson_doc_free(document);
  return context;
}

std::string serializeResponse(const Decision& decision) {
  yyjson_mut_doc* document = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(document);
  yyjson_mut_doc_set_root(document, root);
  yyjson_mut_obj_add_strn(document, root, "action", decision.action.data(), decision.action.size());
  yyjson_mut_obj_add_int(document, root, "amount", decision.amount);
  yyjson_mut_obj_add_strn(document, root, "reason", decision.reason.data(), decision.reason.size());
  if (decision.equity >= 0)
    yyjson_mut_obj_add_real(document, root, "equity", decision.equity);
  if (decision.mdf >= 0)
    yyjson_mut_obj_add_real(document, root, "mdf", decision.mdf);

  size_t length = 0;
  char* output = yyjson_mut_write(document, 0, &length);
  if (!output) {
    yyjson_mut_doc_free(document);
    throw std::runtime_error("failed to serialize v0 response");
  }
  std::string response(output, length);
  std::free(output);
  yyjson_mut_doc_free(document);
  return response;
}

}  // namespace bs::v0
