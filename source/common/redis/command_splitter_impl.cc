#include "common/redis/command_splitter_impl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common/assert.h"
#include "common/redis/supported_commands.h"

#include "fmt/format.h"

namespace Envoy {
namespace Redis {
namespace CommandSplitter {

RespValuePtr Utility::makeError(const std::string& error) {
  RespValuePtr response(new RespValue());
  response->type(RespType::Error);
  response->asString() = error;
  return response;
}

void SplitRequestBase::onWrongNumberOfArguments(SplitCallbacks& callbacks,
                                                const RespValue& request) {
  callbacks.onResponse(Utility::makeError(
      fmt::format("wrong number of arguments for '{}' command", request.asArray()[0].asString())));
}

SingleServerRequest::~SingleServerRequest() { ASSERT(!handle_); }

void SingleServerRequest::onResponse(RespValuePtr&& response) {
  handle_ = nullptr;
  callbacks_.onResponse(std::move(response));
}

void SingleServerRequest::onFailure() {
  handle_ = nullptr;
  callbacks_.onResponse(Utility::makeError("upstream failure"));
}

void SingleServerRequest::cancel() {
  handle_->cancel();
  handle_ = nullptr;
}

SplitRequestPtr SimpleRequest::create(ConnPool::Instance& conn_pool,
                                      const RespValue& incoming_request,
                                      SplitCallbacks& callbacks) {
  std::unique_ptr<SimpleRequest> request_ptr{new SimpleRequest(callbacks)};

  request_ptr->handle_ = conn_pool.makeRequest(incoming_request.asArray()[1].asString(),
                                               incoming_request, *request_ptr);
  if (!request_ptr->handle_) {
    request_ptr->callbacks_.onResponse(Utility::makeError("no upstream host"));
    return nullptr;
  }

  return std::move(request_ptr);
}

SplitRequestPtr EvalRequest::create(ConnPool::Instance& conn_pool,
                                    const RespValue& incoming_request, SplitCallbacks& callbacks) {

  // EVAL looks like: EVAL script numkeys key [key ...] arg [arg ...]
  // Ensure there are at least three args to the command or it cannot be hashed.
  if (incoming_request.asArray().size() < 4) {
    onWrongNumberOfArguments(callbacks, incoming_request);
    return nullptr;
  }

  std::unique_ptr<EvalRequest> request_ptr{new EvalRequest(callbacks)};
  request_ptr->handle_ = conn_pool.makeRequest(incoming_request.asArray()[3].asString(),
                                               incoming_request, *request_ptr);
  if (!request_ptr->handle_) {
    request_ptr->callbacks_.onResponse(Utility::makeError("no upstream host"));
    return nullptr;
  }

  return std::move(request_ptr);
}

FragmentedRequest::~FragmentedRequest() {
#ifndef NDEBUG
  for (const PendingRequest& request : pending_requests_) {
    ASSERT(!request.handle_);
  }
#endif
}

void FragmentedRequest::cancel() {
  for (PendingRequest& request : pending_requests_) {
    if (request.handle_) {
      request.handle_->cancel();
      request.handle_ = nullptr;
    }
  }
}

void FragmentedRequest::onChildFailure(uint32_t index, std::vector<uint32_t> response_indexes) {
  onChildResponse(Utility::makeError("upstream failure"), index, response_indexes);
}

SplitRequestPtr MGETRequest::create(ConnPool::Instance& conn_pool,
                                    const RespValue& incoming_request, SplitCallbacks& callbacks) {
  std::unique_ptr<MGETRequest> request_ptr{new MGETRequest(callbacks)};

  request_ptr->num_pending_responses_ = incoming_request.asArray().size() - 1;
  request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

  request_ptr->pending_response_.reset(new RespValue());
  request_ptr->pending_response_->type(RespType::Array);
  std::vector<RespValue> responses(request_ptr->num_pending_responses_);
  request_ptr->pending_response_->asArray().swap(responses);

  std::vector<RespValue> values(2);
  values[0].type(RespType::BulkString);
  values[0].asString() = "MGET";
  values[1].type(RespType::BulkString);

  RespValue single_mget;
  single_mget.type(RespType::Array);
  single_mget.asArray().swap(values);
  

  
  // first generate a map of unique hosts to hash_key, original_index pairs
  // server1 -> { (foo, 0), (bar, 3), ... }
  std::unordered_map<std::string, std::vector<std::pair<std::string, uint32_t>>> request_map;
  for (uint64_t i = 1; i < incoming_request.asArray().size(); i++) {
    const std::string& hash_key = incoming_request.asArray()[i].asString();
    const std::string& host = conn_pool.getHost(hash_key);
    
    auto key_and_index = std::make_pair(hash_key, i); 
    
    auto collapsed_request = request_map.find(host);
    if (collapsed_request != request_map.end()) {
      collapsed_request->second.push_back(key_and_index);
    } else {
      request_map[host] = {key_and_index};
    }
  }
  
  // now build the requests
  RespValue mget;
  mget.type(RespType::Array);
  uint64_t i = 0;
  for (auto element : request_map ) {
    std::vector<uint32_t> response_indexes;

    std::vector<RespValue> request(element.second.size() + 1);
    request[0].type(RespType::BulkString);
    request[0].asString() = "MGET";
    
    for (uint64_t j = 0; j < element.second.size(); j++) {
      response_indexes.push_back(element.second[j].second);      
      request[j + 1].type(RespType::BulkString);
      request[j + 1].asString() = element.second[j].first;
    }
    mget.asArray().swap(request);

    ENVOY_LOG(info, "redis mget {}: {}", i, mget.toString());
    i++;
  }

  for (uint64_t i = 1; i < incoming_request.asArray().size(); i++) {
    std::vector<uint32_t> response_indexes{static_cast<uint32_t>(i - 1)};

    // need map of hash_key to server
    // then build pairs of request vectors with vector of original indexes
    // map[server] = pair<Request, vector>
    auto hash_key = incoming_request.asArray()[i].asString();
    request_ptr->pending_requests_.emplace_back(*request_ptr, i - 1, response_indexes);
    PendingRequest& pending_request = request_ptr->pending_requests_.back();

    single_mget.asArray()[1].asString() = incoming_request.asArray()[i].asString();
    ENVOY_LOG(debug, "redis: parallel get: '{}'", single_mget.toString());
    pending_request.handle_ = conn_pool.makeRequest(incoming_request.asArray()[i].asString(),
                                                    single_mget, pending_request);
    if (!pending_request.handle_) {
      pending_request.onResponse(Utility::makeError("no upstream host"));
    }
  }

  return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
}

void MGETRequest::onChildResponse(RespValuePtr&& value, uint32_t index,
                                  std::vector<uint32_t> response_indexes) {
  pending_requests_[index].handle_ = nullptr;

  pending_response_->asArray()[index].type(value->type());
  switch (value->type()) {
  case RespType::Integer:
  case RespType::SimpleString: {
    pending_response_->asArray()[index].type(RespType::Error);
    pending_response_->asArray()[index].asString() = "upstream protocol error";
    error_count_++;
    break;
  }
  case RespType::Error:
  case RespType::BulkString: {
    error_count_++;
    pending_response_->asArray()[index].asString().swap(value->asString());
    break;
  }
  case RespType::Array:
    // for real
    for (uint64_t i = 0; i < response_indexes.size(); i++) {
      RespType element_type = value->asArray()[i].type();
      pending_response_->asArray()[index].type(element_type);
      if (element_type != RespType::Null) {
        pending_response_->asArray()[index].asString().swap(value->asArray()[i].asString());
      }
    }
    break;
  case RespType::Null:
    break;
  }

  ASSERT(num_pending_responses_ > 0);
  if (--num_pending_responses_ == 0) {
    ENVOY_LOG(debug, "redis: response: '{}'", pending_response_->toString());
    callbacks_.onResponse(std::move(pending_response_));
  }
}

// SplitRequestPtr MSETRequest::create(ConnPool::Instance& conn_pool,
//                                     const RespValue& incoming_request, SplitCallbacks& callbacks)
//                                     {
//   if ((incoming_request.asArray().size() - 1) % 2 != 0) {
//     onWrongNumberOfArguments(callbacks, incoming_request);
//     return nullptr;
//   }

//   std::unique_ptr<MSETRequest> request_ptr{new MSETRequest(callbacks)};

//   request_ptr->num_pending_responses_ = (incoming_request.asArray().size() - 1) / 2;
//   request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

//   request_ptr->pending_response_.reset(new RespValue());
//   request_ptr->pending_response_->type(RespType::SimpleString);

//   std::vector<RespValue> values(3);
//   values[0].type(RespType::BulkString);
//   values[0].asString() = "set";
//   values[1].type(RespType::BulkString);
//   values[2].type(RespType::BulkString);
//   RespValue single_mset;
//   single_mset.type(RespType::Array);
//   single_mset.asArray().swap(values);

//   uint64_t fragment_index = 0;
//   for (uint64_t i = 1; i < incoming_request.asArray().size(); i += 2) {
//     request_ptr->pending_requests_.emplace_back(*request_ptr, fragment_index++);
//     PendingRequest& pending_request = request_ptr->pending_requests_.back();

//     single_mset.asArray()[1].asString() = incoming_request.asArray()[i].asString();
//     single_mset.asArray()[2].asString() = incoming_request.asArray()[i + 1].asString();

//     ENVOY_LOG(debug, "redis: parallel set: '{}'", single_mset.toString());
//     pending_request.handle_ = conn_pool.makeRequest(incoming_request.asArray()[i].asString(),
//                                                     single_mset, pending_request);
//     if (!pending_request.handle_) {
//       pending_request.onResponse(Utility::makeError("no upstream host"));
//     }
//   }

//   return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
// }

// void MSETRequest::onChildResponse(RespValuePtr&& value, uint32_t index) {
//   pending_requests_[index].handle_ = nullptr;

//   switch (value->type()) {
//   case RespType::SimpleString: {
//     if (value->asString() == "OK") {
//       break;
//     }
//     FALLTHRU;
//   }
//   default: {
//     error_count_++;
//     break;
//   }
//   }

//   ASSERT(num_pending_responses_ > 0);
//   if (--num_pending_responses_ == 0) {
//     if (error_count_ == 0) {
//       pending_response_->asString() = "OK";
//       callbacks_.onResponse(std::move(pending_response_));
//     } else {
//       callbacks_.onResponse(
//           Utility::makeError(fmt::format("finished with {} error(s)", error_count_)));
//     }
//   }
// }

// SplitRequestPtr SplitKeysSumResultRequest::create(ConnPool::Instance& conn_pool,
//                                                   const RespValue& incoming_request,
//                                                   SplitCallbacks& callbacks) {
//   std::unique_ptr<SplitKeysSumResultRequest> request_ptr{new
//   SplitKeysSumResultRequest(callbacks)};

//   request_ptr->num_pending_responses_ = incoming_request.asArray().size() - 1;
//   request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

//   request_ptr->pending_response_.reset(new RespValue());
//   request_ptr->pending_response_->type(RespType::Integer);

//   std::vector<RespValue> values(2);
//   values[0].type(RespType::BulkString);
//   values[0].asString() = incoming_request.asArray()[0].asString();
//   values[1].type(RespType::BulkString);
//   RespValue single_fragment;
//   single_fragment.type(RespType::Array);
//   single_fragment.asArray().swap(values);

//   for (uint64_t i = 1; i < incoming_request.asArray().size(); i++) {
//     request_ptr->pending_requests_.emplace_back(*request_ptr, i - 1);
//     PendingRequest& pending_request = request_ptr->pending_requests_.back();

//     single_fragment.asArray()[1].asString() = incoming_request.asArray()[i].asString();
//     ENVOY_LOG(debug, "redis: parallel {}: '{}'", incoming_request.asArray()[0].asString(),
//               single_fragment.toString());
//     pending_request.handle_ = conn_pool.makeRequest(incoming_request.asArray()[i].asString(),
//                                                     single_fragment, pending_request);
//     if (!pending_request.handle_) {
//       pending_request.onResponse(Utility::makeError("no upstream host"));
//     }
//   }

//   return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
// }

// void SplitKeysSumResultRequest::onChildResponse(RespValuePtr&& value, uint32_t index) {
//   pending_requests_[index].handle_ = nullptr;

//   switch (value->type()) {
//   case RespType::Integer: {
//     total_ += value->asInteger();
//     break;
//   }
//   default: {
//     error_count_++;
//     break;
//   }
//   }

//   ASSERT(num_pending_responses_ > 0);
//   if (--num_pending_responses_ == 0) {
//     if (error_count_ == 0) {
//       pending_response_->asInteger() = total_;
//       callbacks_.onResponse(std::move(pending_response_));
//     } else {
//       callbacks_.onResponse(
//           Utility::makeError(fmt::format("finished with {} error(s)", error_count_)));
//     }
//   }
// }

InstanceImpl::InstanceImpl(ConnPool::InstancePtr&& conn_pool, Stats::Scope& scope,
                           const std::string& stat_prefix)
    : conn_pool_(std::move(conn_pool)), simple_command_handler_(*conn_pool_),
      eval_command_handler_(*conn_pool_),
      mget_handler_(*conn_pool_), stats_{ALL_COMMAND_SPLITTER_STATS(
                                      POOL_COUNTER_PREFIX(scope, stat_prefix + "splitter."))} {

  // InstanceImpl::InstanceImpl(ConnPool::InstancePtr&& conn_pool, Stats::Scope& scope,
  //                            const std::string& stat_prefix)
  //     : conn_pool_(std::move(conn_pool)), simple_command_handler_(*conn_pool_),
  //       eval_command_handler_(*conn_pool_), mget_handler_(*conn_pool_),
  //       mset_handler_(*conn_pool_), split_keys_sum_result_handler_(*conn_pool_),
  //       stats_{ALL_COMMAND_SPLITTER_STATS(POOL_COUNTER_PREFIX(scope, stat_prefix + "splitter."))}
  //       {

  // TODO(mattklein123) PERF: Make this a trie (like in header_map_impl).
  for (const std::string& command : SupportedCommands::simpleCommands()) {
    addHandler(scope, stat_prefix, command, simple_command_handler_);
  }

  for (const std::string& command : SupportedCommands::evalCommands()) {
    addHandler(scope, stat_prefix, command, eval_command_handler_);
  }

  // for (const std::string& command : SupportedCommands::hashMultipleSumResultCommands()) {
  //   addHandler(scope, stat_prefix, command, split_keys_sum_result_handler_);
  // }

  addHandler(scope, stat_prefix, SupportedCommands::mget(), mget_handler_);
  // addHandler(scope, stat_prefix, SupportedCommands::mset(), mset_handler_);
}

SplitRequestPtr InstanceImpl::makeRequest(const RespValue& request, SplitCallbacks& callbacks) {
  if (request.type() != RespType::Array || request.asArray().size() < 2) {
    onInvalidRequest(callbacks);
    return nullptr;
  }

  for (const RespValue& value : request.asArray()) {
    if (value.type() != RespType::BulkString) {
      onInvalidRequest(callbacks);
      return nullptr;
    }
  }

  std::string to_lower_string(request.asArray()[0].asString());
  to_lower_table_.toLowerCase(to_lower_string);

  auto handler = command_map_.find(to_lower_string);
  if (handler == command_map_.end()) {
    stats_.unsupported_command_.inc();
    callbacks.onResponse(Utility::makeError(
        fmt::format("unsupported command '{}'", request.asArray()[0].asString())));
    return nullptr;
  }

  ENVOY_LOG(debug, "redis: splitting '{}'", request.toString());
  handler->second.total_.inc();
  return handler->second.handler_.get().startRequest(request, callbacks);
}

void InstanceImpl::onInvalidRequest(SplitCallbacks& callbacks) {
  stats_.invalid_request_.inc();
  callbacks.onResponse(Utility::makeError("invalid request"));
}

void InstanceImpl::addHandler(Stats::Scope& scope, const std::string& stat_prefix,
                              const std::string& name, CommandHandler& handler) {
  std::string to_lower_name(name);
  to_lower_table_.toLowerCase(to_lower_name);
  command_map_.emplace(
      to_lower_name,
      HandlerData{scope.counter(fmt::format("{}command.{}.total", stat_prefix, to_lower_name)),
                  handler});
}

} // namespace CommandSplitter
} // namespace Redis
} // namespace Envoy
