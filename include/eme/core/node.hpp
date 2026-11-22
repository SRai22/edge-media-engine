#pragma once

#include <optional>
#include <string>
#include <utility>

#include "eme/core/distributor.hpp"
#include "eme/core/types.hpp"

namespace eme {

class Node {
 public:
  explicit Node(std::string name) : name_(std::move(name)) {}
  virtual ~Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  const std::string& name() const noexcept { return name_; }

  virtual std::optional<VideoFormat> outputFormat() const { return std::nullopt; }
  virtual bool accepts(const VideoFormat&) const { return false; }

  virtual Result start() { return {}; }
  virtual void stop() noexcept {}

 private:
  std::string name_;
};

template <class T>
class Producer {
 public:
  virtual ~Producer() = default;
  virtual Distributor<T>& output() = 0;
};

template <class T>
class Consumer {
 public:
  virtual ~Consumer() = default;
  virtual Result bindInput(Distributor<T>& upstream) = 0;
};

}  // namespace eme
