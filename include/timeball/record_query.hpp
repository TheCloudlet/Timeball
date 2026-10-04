// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_RECORD_QUERY_HPP
#define TIMEBALL_RECORD_QUERY_HPP

#include <map>
#include <ranges>
#include <type_traits>
#include <utility>

#include "timeball/event_engine.hpp"

// Filter, group, and fold over records. With a range, returns the answer.
// Without one, returns a RecordSink that folds each record and drops it. A
// group key must outlive the sink. Return an owning string if it does not.

namespace timeball {

template <typename Acc, typename Fn>
class FoldSink final : public RecordSink {
 public:
  FoldSink(Acc init, Fn fn) : acc_(std::move(init)), fn_(std::move(fn)) {}

  void OnRecord(const Record& record) override {
    acc_ = fn_(std::move(acc_), record);
  }

  [[nodiscard]] const Acc& value() const { return acc_; }

 private:
  Acc acc_;
  Fn fn_;
};

template <typename Pred>
class FilterSink final : public RecordSink {
 public:
  FilterSink(Pred pred, RecordSink& next)
      : pred_(std::move(pred)), next_(&next) {}

  void OnRecord(const Record& record) override {
    if (pred_(record)) {
      next_->OnRecord(record);
    }
  }

 private:
  Pred pred_;
  RecordSink* next_;
};

template <typename Key, typename Acc, typename Fn>
class GroupSink final : public RecordSink {
 public:
  using KeyType = std::decay_t<std::invoke_result_t<Key&, const Record&>>;

  GroupSink(Key key, Acc init, Fn fn)
      : key_(std::move(key)), init_(std::move(init)), fn_(std::move(fn)) {}

  void OnRecord(const Record& record) override {
    auto [it, inserted] = groups_.try_emplace(key_(record), init_);
    (void)inserted;
    it->second = fn_(std::move(it->second), record);
  }

  [[nodiscard]] const std::map<KeyType, Acc>& value() const { return groups_; }

 private:
  Key key_;
  Acc init_;
  Fn fn_;
  std::map<KeyType, Acc> groups_;
};

template <typename Acc, typename Fn>
FoldSink<std::decay_t<Acc>, std::decay_t<Fn>> Fold(Acc init, Fn fn) {
  return {std::move(init), std::move(fn)};
}

template <typename Pred>
FilterSink<std::decay_t<Pred>> Filter(Pred pred, RecordSink& next) {
  return {std::move(pred), next};
}

template <typename Key, typename Acc, typename Fn>
GroupSink<std::decay_t<Key>, std::decay_t<Acc>, std::decay_t<Fn>> Group(
    Key key, Acc init, Fn fn) {
  return {std::move(key), std::move(init), std::move(fn)};
}

namespace query_detail {

template <typename Sink, std::ranges::input_range R>
void Feed(Sink& sink, R&& records) {
  for (const Record& record : records) {
    sink.OnRecord(record);
  }
}

}  // namespace query_detail

template <std::ranges::input_range R, typename Acc, typename Fn>
Acc Fold(R&& records, Acc init, Fn fn) {
  auto sink = Fold(std::move(init), std::move(fn));
  query_detail::Feed(sink, records);
  return sink.value();
}

template <std::ranges::viewable_range R, typename Pred>
auto Filter(R&& records, Pred pred) {
  return std::forward<R>(records) | std::views::filter(std::move(pred));
}

template <std::ranges::input_range R, typename Key, typename Acc, typename Fn>
auto Group(R&& records, Key key, Acc init, Fn fn) {
  auto sink = Group(std::move(key), std::move(init), std::move(fn));
  query_detail::Feed(sink, records);
  return sink.value();
}

}  // namespace timeball

#endif  // TIMEBALL_RECORD_QUERY_HPP
