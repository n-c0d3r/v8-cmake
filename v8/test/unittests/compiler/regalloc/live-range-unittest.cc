// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/backend/register-allocator.h"
#include "test/unittests/test-utils.h"

// TODO(mtrofin): would we want to centralize this definition?
#ifdef DEBUG
#define V8_ASSERT_DEBUG_DEATH(statement, regex) \
  ASSERT_DEATH_IF_SUPPORTED(statement, regex)
#define DISABLE_IN_RELEASE(Name) Name

#else
#define V8_ASSERT_DEBUG_DEATH(statement, regex) statement
#define DISABLE_IN_RELEASE(Name) DISABLED_##Name
#endif  // DEBUG

namespace v8 {
namespace internal {
namespace compiler {

// Utility offering shorthand syntax for building up a range by providing its ID
// and pairs (start, end) specifying intervals. Circumvents current incomplete
// support for C++ features such as instantiation lists, on OS X and Android.
class TestRangeBuilder {
 public:
  explicit TestRangeBuilder(Zone* zone)
      : id_(-1), pairs_(), uses_(), zone_(zone) {}

  TestRangeBuilder& Id(int id) {
    id_ = id;
    return *this;
  }
  TestRangeBuilder& Add(int start, int end) {
    pairs_.push_back({start, end});
    return *this;
  }

  TestRangeBuilder& AddUse(int pos) {
    uses_.insert(pos);
    return *this;
  }

  TopLevelLiveRange* Build(int start, int end) {
    return Add(start, end).Build();
  }

  TopLevelLiveRange* Build() {
    TopLevelLiveRange* range = zone_->New<TopLevelLiveRange>(
        id_, MachineRepresentation::kTagged, zone_);
    // Traverse the provided interval specifications backwards, because that is
    // what LiveRange expects.
    for (int i = static_cast<int>(pairs_.size()) - 1; i >= 0; --i) {
      Interval pair = pairs_[i];
      LifetimePosition start = LifetimePosition::FromInt(pair.first);
      LifetimePosition end = LifetimePosition::FromInt(pair.second);
      CHECK(start < end);
      range->AddUseInterval(start, end, zone_, v8_flags.trace_turbo_alloc);
    }
    for (int pos : uses_) {
      UsePosition* use_position =
          zone_->New<UsePosition>(LifetimePosition::FromInt(pos), nullptr,
                                  nullptr, UsePositionHintType::kNone);
      range->AddUsePosition(use_position, v8_flags.trace_turbo_alloc);
    }

    pairs_.clear();
    return range;
  }

 private:
  using Interval = std::pair<int, int>;
  using IntervalList = std::vector<Interval>;
  int id_;
  IntervalList pairs_;
  std::set<int> uses_;
  Zone* zone_;
};

class LiveRangeUnitTest : public TestWithZone {
 public:
  // Split helper, to avoid int->LifetimePosition conversion nuisance.
  LiveRange* Split(LiveRange* range, int pos) {
    return range->SplitAt(LifetimePosition::FromInt(pos), zone());
  }

  // Ranges first and second match structurally.
  bool RangesMatch(LiveRange* first, LiveRange* second) {
    if (first->Start() != second->Start() || first->End() != second->End()) {
      return false;
    }
    UseInterval* i1 = first->first_interval();
    UseInterval* i2 = second->first_interval();

    while (i1 != nullptr && i2 != nullptr) {
      if (i1->start() != i2->start() || i1->end() != i2->end()) return false;
      i1 = i1->next();
      i2 = i2->next();
    }
    if (i1 != nullptr || i2 != nullptr) return false;

    UsePosition* const* p1 = first->positions().begin();
    UsePosition* const* p2 = second->positions().begin();

    while (p1 != first->positions().end() && p2 != second->positions().end()) {
      if ((*p1)->pos() != (*p2)->pos()) return false;
      ++p1;
      ++p2;
    }
    if (p1 != first->positions().end() || p2 != second->positions().end()) {
      return false;
    }
    return true;
  }
};

TEST_F(LiveRangeUnitTest, InvalidConstruction) {
  // Build a range manually, because the builder guards against empty cases.
  TopLevelLiveRange* range =
      zone()->New<TopLevelLiveRange>(1, MachineRepresentation::kTagged, zone());
  V8_ASSERT_DEBUG_DEATH(
      range->AddUseInterval(LifetimePosition::FromInt(0),
                            LifetimePosition::FromInt(0), zone(),
                            v8_flags.trace_turbo_alloc),
      ".*");
}

TEST_F(LiveRangeUnitTest, SplitInvalidStart) {
  TopLevelLiveRange* range = TestRangeBuilder(zone()).Build(0, 1);
  V8_ASSERT_DEBUG_DEATH(Split(range, 0), ".*");
}

TEST_F(LiveRangeUnitTest, DISABLE_IN_RELEASE(InvalidSplitEnd)) {
  TopLevelLiveRange* range = TestRangeBuilder(zone()).Build(0, 1);
  ASSERT_DEATH_IF_SUPPORTED(Split(range, 1), ".*");
}

TEST_F(LiveRangeUnitTest, DISABLE_IN_RELEASE(SplitInvalidPreStart)) {
  TopLevelLiveRange* range = TestRangeBuilder(zone()).Build(1, 2);
  ASSERT_DEATH_IF_SUPPORTED(Split(range, 0), ".*");
}

TEST_F(LiveRangeUnitTest, DISABLE_IN_RELEASE(SplitInvalidPostEnd)) {
  TopLevelLiveRange* range = TestRangeBuilder(zone()).Build(0, 1);
  ASSERT_DEATH_IF_SUPPORTED(Split(range, 2), ".*");
}

TEST_F(LiveRangeUnitTest, SplitSingleIntervalNoUsePositions) {
  TopLevelLiveRange* range = TestRangeBuilder(zone()).Build(0, 2);
  LiveRange* child = Split(range, 1);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top = TestRangeBuilder(zone()).Build(0, 1);
  LiveRange* expected_bottom = TestRangeBuilder(zone()).Build(1, 2);
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalNoUsePositionsBetween) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).Build();
  LiveRange* child = Split(range, 3);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top = TestRangeBuilder(zone()).Build(0, 2);
  LiveRange* expected_bottom = TestRangeBuilder(zone()).Build(4, 6);
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalNoUsePositionsFront) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).Build();
  LiveRange* child = Split(range, 1);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top = TestRangeBuilder(zone()).Build(0, 1);
  LiveRange* expected_bottom =
      TestRangeBuilder(zone()).Add(1, 2).Add(4, 6).Build();
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalNoUsePositionsAfter) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).Build();
  LiveRange* child = Split(range, 5);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 5).Build();
  LiveRange* expected_bottom = TestRangeBuilder(zone()).Build(5, 6);
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitSingleIntervalUsePositions) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 3).AddUse(0).AddUse(2).Build();

  LiveRange* child = Split(range, 1);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 1).AddUse(0).Build();
  LiveRange* expected_bottom =
      TestRangeBuilder(zone()).Add(1, 3).AddUse(2).Build();
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitSingleIntervalUsePositionsAtPos) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 3).AddUse(0).AddUse(2).Build();

  LiveRange* child = Split(range, 2);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 2).AddUse(0).AddUse(2).Build();
  LiveRange* expected_bottom = TestRangeBuilder(zone()).Build(2, 3);
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalUsePositionsBetween) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).AddUse(1).AddUse(5).Build();
  LiveRange* child = Split(range, 3);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 2).AddUse(1).Build();
  LiveRange* expected_bottom =
      TestRangeBuilder(zone()).Add(4, 6).AddUse(5).Build();
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalUsePositionsAtInterval) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).AddUse(1).AddUse(4).Build();
  LiveRange* child = Split(range, 4);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 2).AddUse(1).Build();
  LiveRange* expected_bottom =
      TestRangeBuilder(zone()).Add(4, 6).AddUse(4).Build();
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalUsePositionsFront) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).AddUse(1).AddUse(5).Build();
  LiveRange* child = Split(range, 1);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 1).AddUse(1).Build();
  LiveRange* expected_bottom =
      TestRangeBuilder(zone()).Add(1, 2).Add(4, 6).AddUse(5).Build();
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

TEST_F(LiveRangeUnitTest, SplitManyIntervalUsePositionsAfter) {
  TopLevelLiveRange* range =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 6).AddUse(1).AddUse(5).Build();
  LiveRange* child = Split(range, 5);

  EXPECT_NE(nullptr, range->next());
  EXPECT_EQ(child, range->next());

  LiveRange* expected_top =
      TestRangeBuilder(zone()).Add(0, 2).Add(4, 5).AddUse(1).AddUse(5).Build();
  LiveRange* expected_bottom = TestRangeBuilder(zone()).Build(5, 6);
  EXPECT_TRUE(RangesMatch(expected_top, range));
  EXPECT_TRUE(RangesMatch(expected_bottom, child));
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
