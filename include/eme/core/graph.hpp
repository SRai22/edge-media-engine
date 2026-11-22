#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "eme/core/node.hpp"

namespace eme {

// Static DAG: build with add()/connect(), then start(). Consumers start before producers and
// producers stop before consumers, so no frame is published into a node that is not running.
class MediaGraph {
 public:
  MediaGraph() = default;
  ~MediaGraph() { stop(); }
  MediaGraph(const MediaGraph&) = delete;
  MediaGraph& operator=(const MediaGraph&) = delete;

  template <class N, class... Args>
  N& add(Args&&... args) {
    auto node = std::make_unique<N>(std::forward<Args>(args)...);
    N& ref = *node;
    nodes_.push_back(std::move(node));
    return ref;
  }

  Result connect(Node& from, Node& to);
  Result start();
  void stop() noexcept;
  bool running() const noexcept { return running_; }

 private:
  std::optional<std::size_t> indexOf(const Node& node) const;
  bool reachable(std::size_t from, std::size_t to) const;
  std::vector<std::size_t> topologicalOrder() const;

  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::pair<std::size_t, std::size_t>> edges_;
  std::vector<std::size_t> started_;  // in start order
  bool running_ = false;
};

}  // namespace eme
