/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <qti/qimsdk-element.h>

namespace qti {

class ElementRegistry {
public:
  using Creator = std::function<std::unique_ptr<Element>(const std::string& factory,
    const std::string& name)>;

  static ElementRegistry& Instance();

  void Register(const std::string& factory, Creator creator);
  std::unique_ptr<Element> Create(const std::string& factory,
    const std::string& name) const;

private:
  ElementRegistry();
  ~ElementRegistry() = default;

  ElementRegistry(const ElementRegistry&) = delete;
  ElementRegistry& operator=(const ElementRegistry&) = delete;

  mutable std::mutex mtx_;
  std::unordered_map<std::string, Creator> creators_;
};

}  // namespace qti
