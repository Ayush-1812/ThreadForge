#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

class SubmissionCopyError : public std::runtime_error {
public:
    SubmissionCopyError() : std::runtime_error("submission copy failed") {}
};

class SubmissionMoveError : public std::runtime_error {
public:
    SubmissionMoveError() : std::runtime_error("submission move failed") {}
};

struct ThrowingCallableCopy {
    explicit ThrowingCallableCopy(std::atomic<int>& executions) : executions_(&executions) {}
    ThrowingCallableCopy(const ThrowingCallableCopy& other) : executions_(other.executions_) {
        throw SubmissionCopyError{};
    }
    ThrowingCallableCopy(ThrowingCallableCopy&&) = default;
    int operator()() const {
        ++*executions_;
        return 7;
    }
private:
    std::atomic<int>* executions_;
};

struct ThrowingCallableMove {
    explicit ThrowingCallableMove(std::atomic<int>& executions) : executions_(&executions) {}
    ThrowingCallableMove(const ThrowingCallableMove&) = delete;
    ThrowingCallableMove(ThrowingCallableMove&& other) : executions_(other.executions_) {
        throw SubmissionMoveError{};
    }
    int operator()() && {
        ++*executions_;
        return 11;
    }
private:
    std::atomic<int>* executions_;
};

struct ThrowingArgumentCopy {
    ThrowingArgumentCopy() = default;
    ThrowingArgumentCopy(const ThrowingArgumentCopy&) { throw SubmissionCopyError{}; }
    ThrowingArgumentCopy(ThrowingArgumentCopy&&) = default;
};

struct ThrowingArgumentMove {
    ThrowingArgumentMove() = default;
    ThrowingArgumentMove(const ThrowingArgumentMove&) = delete;
    ThrowingArgumentMove(ThrowingArgumentMove&&) { throw SubmissionMoveError{}; }
};

class ResultStorageError : public std::runtime_error {
public:
    ResultStorageError() : std::runtime_error("result storage move failed") {}
};

struct ThrowingResultMove {
    explicit ThrowingResultMove(std::atomic<int>& moves) : moves_(&moves) {}
    ThrowingResultMove(const ThrowingResultMove&) = delete;
    ThrowingResultMove(ThrowingResultMove&& other) : moves_(other.moves_) {
        ++*moves_;
        throw ResultStorageError{};
    }
private:
    std::atomic<int>* moves_;
};

struct OnDestruction {
    explicit OnDestruction(std::function<void()> action) : action_(std::move(action)) {}
    ~OnDestruction() { action_(); }
private:
    std::function<void()> action_;
};

void check_unadmitted_and_recovery(threadforge::Scheduler& scheduler, std::atomic<int>& executions) {
    const auto rejected = scheduler.metrics();
    EXPECT_EQ(rejected.tasks_submitted, 0U);
    EXPECT_EQ(rejected.tasks_pending, 0U);
    EXPECT_EQ(rejected.tasks_started, 0U);
    EXPECT_EQ(rejected.tasks_failed, 0U);
    EXPECT_EQ(rejected.peak_tasks_pending, 0U);
    EXPECT_EQ(executions.load(), 0);
    EXPECT_EQ(scheduler.submit([] { return 19; }).get(), 19);
    scheduler.shutdown();
    const auto recovered = scheduler.metrics();
    EXPECT_EQ(recovered.tasks_submitted, 1U);
    EXPECT_EQ(recovered.tasks_completed, 1U);
    EXPECT_EQ(recovered.tasks_failed, 0U);
}

TEST(Hardening, ThrowingCallableCopyRejectsBeforeAdmissionAndSchedulerRecovers) {
    std::atomic<int> executions{0};
    threadforge::Scheduler scheduler(1);
    ThrowingCallableCopy callable(executions);
    EXPECT_THROW(scheduler.submit(callable), SubmissionCopyError);
    check_unadmitted_and_recovery(scheduler, executions);
}

TEST(Hardening, ThrowingCallableMoveRejectsBeforeAdmissionAndSchedulerRecovers) {
    std::atomic<int> executions{0};
    threadforge::Scheduler scheduler(1);
    ThrowingCallableMove callable(executions);
    EXPECT_THROW(scheduler.submit(std::move(callable)), SubmissionMoveError);
    check_unadmitted_and_recovery(scheduler, executions);
}

TEST(Hardening, ThrowingArgumentCopyRejectsBeforeAdmissionAndSchedulerRecovers) {
    std::atomic<int> executions{0};
    threadforge::Scheduler scheduler(1);
    ThrowingArgumentCopy argument;
    EXPECT_THROW(scheduler.submit([&](ThrowingArgumentCopy) { ++executions; }, argument), SubmissionCopyError);
    check_unadmitted_and_recovery(scheduler, executions);
}

TEST(Hardening, ThrowingArgumentMoveRejectsBeforeAdmissionAndSchedulerRecovers) {
    std::atomic<int> executions{0};
    threadforge::Scheduler scheduler(1);
    ThrowingArgumentMove argument;
    EXPECT_THROW(scheduler.submit([&](ThrowingArgumentMove) { ++executions; }, std::move(argument)), SubmissionMoveError);
    check_unadmitted_and_recovery(scheduler, executions);
}

TEST(Hardening, ExceptionalResultStorageReleasesPendingDependentsWithoutCountingCallableFailure) {
    std::atomic<int> moves{0};
    std::atomic<int> invoked{0};
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task([&, gate] {
        entered.count_down();
        gate.wait();
        ++invoked;
        // Mandatory prvalue elision defers the throwing move to result storage.
        return ThrowingResultMove{moves};
    });
    entered.wait();
    auto root_value = root.future.share();
    auto child = scheduler.submit_after({root.handle}, [root_value] {
        try {
            static_cast<void>(root_value.get());
        } catch (const ResultStorageError&) {
            return 23;
        }
        return 0;
    });
    const auto waiting = scheduler.metrics();
    release.set_value();
    EXPECT_EQ(child.future.get(), 23);
    EXPECT_THROW(root_value.get(), ResultStorageError);
    scheduler.shutdown();
    EXPECT_EQ(waiting.tasks_waiting_dependencies, 1U);
    EXPECT_EQ(invoked.load(), 1);
    EXPECT_EQ(moves.load(), 1);
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.tasks_submitted, 2U);
    EXPECT_EQ(finished.tasks_completed, 2U);
    EXPECT_EQ(finished.tasks_failed, 0U);
    EXPECT_EQ(finished.tasks_pending, 0U);
}

TEST(Hardening, DiscardedQueuedFutureKeepsCaptureAliveUntilItsTaskExecutes) {
    std::atomic<int> executions{0};
    std::atomic<int> destructions{0};
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit([&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    auto payload = std::make_unique<OnDestruction>([&] { ++destructions; });
    auto queued = scheduler.submit([&, payload = std::move(payload)] {
        if (payload) {
            ++executions;
        }
    });
    queued = std::future<void>{};
    const int before_execution = destructions.load();
    release.set_value();
    blocker.get();
    scheduler.shutdown();
    EXPECT_EQ(before_execution, 0);
    EXPECT_EQ(executions.load(), 1);
    EXPECT_EQ(destructions.load(), 1);
    EXPECT_EQ(scheduler.metrics().tasks_completed, 2U);
}

TEST(Hardening, CompletedCaptureDestructorCanReadMetricsAndSubmitFollowupWork) {
    std::atomic<int> destructions{0};
    std::atomic<int> followups{0};
    std::promise<std::future<int>> published;
    auto publication = published.get_future();
    threadforge::Scheduler scheduler(1);
    auto payload = std::make_unique<OnDestruction>([&] {
        try {
            static_cast<void>(scheduler.metrics());
            ++destructions;
            published.set_value(scheduler.submit([&] { ++followups; return 29; }));
        } catch (...) {
            published.set_exception(std::current_exception());
        }
    });
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate, payload = std::move(payload)] {
        entered.count_down();
        gate.wait();
        static_cast<void>(payload);
    });
    entered.wait();
    parent = std::future<void>{};
    release.set_value();
    auto followup = publication.get();
    EXPECT_EQ(followup.get(), 29);
    scheduler.shutdown();
    EXPECT_EQ(destructions.load(), 1);
    EXPECT_EQ(followups.load(), 1);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 2U);
    EXPECT_EQ(scheduler.metrics().tasks_completed, 2U);
}

TEST(Hardening, InvalidDependencyCleanupCanReadMetricsAndReenterSubmission) {
    std::atomic<int> destructions{0};
    std::promise<std::future<int>> published;
    auto publication = published.get_future();
    threadforge::Scheduler scheduler(1);
    auto payload = std::make_unique<OnDestruction>([&] {
        try {
            static_cast<void>(scheduler.metrics());
            ++destructions;
            published.set_value(scheduler.submit([] { return 31; }));
        } catch (...) {
            published.set_exception(std::current_exception());
        }
    });
    EXPECT_THROW(scheduler.submit_after({threadforge::TaskHandle{}}, [payload = std::move(payload)] {
        static_cast<void>(payload);
    }), std::invalid_argument);
    auto followup = publication.get();
    EXPECT_EQ(followup.get(), 31);
    scheduler.shutdown();
    EXPECT_EQ(destructions.load(), 1);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 1U);
    EXPECT_EQ(scheduler.metrics().tasks_completed, 1U);
    EXPECT_EQ(scheduler.metrics().tasks_failed, 0U);
}

TEST(Hardening, ClosedAdmissionCleanupCanReenterMetricsAndObserveSubmissionRejection) {
    std::promise<bool> cleaned;
    auto cleanup = cleaned.get_future();
    threadforge::Scheduler scheduler(1);
    scheduler.shutdown();
    auto payload = std::make_unique<OnDestruction>([&] {
        try {
            const auto snapshot = scheduler.metrics();
            try {
                static_cast<void>(scheduler.submit([] {}));
            } catch (const std::runtime_error&) {
                cleaned.set_value(snapshot.tasks_submitted == 0);
                return;
            }
            cleaned.set_value(false);
        } catch (...) {
            cleaned.set_exception(std::current_exception());
        }
    });
    EXPECT_THROW(scheduler.submit([payload = std::move(payload)] { static_cast<void>(payload); }), std::runtime_error);
    EXPECT_TRUE(cleanup.get());
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 0U);
    EXPECT_EQ(scheduler.metrics().tasks_failed, 0U);
}

TEST(Hardening, RvalueQualifiedCallableReturnsMoveOnlyValueReadableByDependent) {
    struct RvalueOnly {
        std::unique_ptr<int> value;
        std::unique_ptr<int> operator()() && { return std::move(value); }
        std::unique_ptr<int> operator()() & = delete;
    };
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task(RvalueOnly{std::make_unique<int>(37)});
    auto value = root.future.share();
    auto child = scheduler.submit_after({root.handle}, [value] { return *value.get() + 4; });
    EXPECT_EQ(child.future.get(), 41);
    EXPECT_EQ(*value.get(), 37);
    scheduler.shutdown();
    EXPECT_EQ(scheduler.metrics().tasks_completed, 2U);
    EXPECT_EQ(scheduler.metrics().tasks_failed, 0U);
}

} // namespace
