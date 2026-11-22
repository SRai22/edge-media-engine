#include "eme/core/graph.hpp"

#include <spdlog/spdlog.h>

namespace eme {
namespace {

template <class T>
std::optional<Result> tryBind(Node& from, Node& to) {
  auto* producer = dynamic_cast<Producer<T>*>(&from);
  auto* consumer = dynamic_cast<Consumer<T>*>(&to);
  if (!producer || !consumer) return std::nullopt;
  return consumer->bindInput(producer->output());
}

}  // namespace

Result MediaGraph::connect(Node& from, Node& to) {
  if (running_) return fail(ErrorCode::InvalidState, "graph is running; V1 graphs are static");
  const auto a = indexOf(from);
  const auto b = indexOf(to);
  if (!a || !b) return fail(ErrorCode::InvalidArgument, "node is not owned by this graph");
  if (*a == *b || reachable(*b, *a)) {
    return fail(ErrorCode::Cycle, from.name() + " -> " + to.name() + " would create a cycle");
  }
  const auto format = from.outputFormat();
  if (!format) return fail(ErrorCode::Incompatible, from.name() + " has no output");
  if (!to.accepts(*format)) {
    return fail(ErrorCode::Incompatible, to.name() + " does not accept the output of " + from.name());
  }
  const auto bound = format->kind == MediaKind::RawVideo ? tryBind<VideoFrame>(from, to)
                                                         : tryBind<EncodedVideoFrame>(from, to);
  if (!bound) return fail(ErrorCode::Incompatible, from.name() + " -> " + to.name() + ": port types differ");
  if (!*bound) return *bound;
  edges_.emplace_back(*a, *b);
  return {};
}

Result MediaGraph::start() {
  if (running_) return fail(ErrorCode::InvalidState, "graph already running");
  const auto order = topologicalOrder();
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    Node& node = *nodes_[*it];
    if (auto r = node.start(); !r) {
      spdlog::error("graph: {} failed to start: {}", node.name(), r.error().message);
      stop();
      return r;
    }
    started_.push_back(*it);
  }
  running_ = true;
  spdlog::info("graph: started {} nodes", nodes_.size());
  return {};
}

void MediaGraph::stop() noexcept {
  for (auto it = started_.rbegin(); it != started_.rend(); ++it) nodes_[*it]->stop();
  if (running_) spdlog::info("graph: stopped");
  started_.clear();
  running_ = false;
}

std::optional<std::size_t> MediaGraph::indexOf(const Node& node) const {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].get() == &node) return i;
  }
  return std::nullopt;
}

bool MediaGraph::reachable(std::size_t from, std::size_t to) const {
  std::vector<bool> seen(nodes_.size());
  std::vector<std::size_t> stack{from};
  while (!stack.empty()) {
    const auto n = stack.back();
    stack.pop_back();
    if (n == to) return true;
    if (seen[n]) continue;
    seen[n] = true;
    for (const auto& [src, dst] : edges_) {
      if (src == n) stack.push_back(dst);
    }
  }
  return false;
}

std::vector<std::size_t> MediaGraph::topologicalOrder() const {
  std::vector<std::size_t> inDegree(nodes_.size()), order, ready;
  for (const auto& edge : edges_) ++inDegree[edge.second];
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (inDegree[i] == 0) ready.push_back(i);
  }
  while (!ready.empty()) {
    const auto n = ready.back();
    ready.pop_back();
    order.push_back(n);
    for (const auto& [src, dst] : edges_) {
      if (src == n && --inDegree[dst] == 0) ready.push_back(dst);
    }
  }
  return order;  // complete: connect() rejects cycles
}

}  // namespace eme
