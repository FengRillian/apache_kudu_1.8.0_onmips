// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/consensus/leader_election.h"

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus-test-util.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus_peers.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/threadpool.h"

namespace kudu {

namespace rpc {
class Messenger;
} // namespace rpc

namespace consensus {

using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;

namespace {

const int kLeaderElectionTimeoutSecs = 10;

// Generate list of voter uuids.
vector<string> GenVoterUUIDs(int num_voters) {
  vector<string> voter_uuids;
  for (int i = 0; i < num_voters; i++) {
    voter_uuids.push_back(Substitute("peer-$0", i));
  }
  return voter_uuids;
}

} // namespace

////////////////////////////////////////
// LeaderElectionTest
////////////////////////////////////////

typedef unordered_map<string, PeerProxy*> ProxyMap;

// A proxy factory that serves proxies from a map.
class FromMapPeerProxyFactory : public PeerProxyFactory {
 public:
  explicit FromMapPeerProxyFactory(const ProxyMap* proxy_map)
      : proxy_map_(proxy_map) {
  }

  Status NewProxy(const RaftPeerPB& peer_pb,
                  gscoped_ptr<PeerProxy>* proxy) override {
    PeerProxy* proxy_ptr = FindPtrOrNull(*proxy_map_, peer_pb.permanent_uuid());
    if (!proxy_ptr) return Status::NotFound("no proxy for peer");
    proxy->reset(proxy_ptr);
    return Status::OK();
  }

  const shared_ptr<rpc::Messenger>& messenger() const override {
    return null_messenger_;
  }

 private:
  // FYI, the tests may add and remove nodes from this map while we hold a
  // reference to it.
  const ProxyMap* const proxy_map_;

  shared_ptr<rpc::Messenger> null_messenger_;
};

class LeaderElectionTest : public KuduTest {
 public:
  LeaderElectionTest()
    : tablet_id_("test-tablet"),
      proxy_factory_(new FromMapPeerProxyFactory(&proxies_)),
      latch_(1) {
    CHECK_OK(ThreadPoolBuilder("test-peer-pool").set_max_threads(5).Build(&pool_));
  }

  void ElectionCallback(const ElectionResult& result);

 protected:
  void InitUUIDs(int num_voters);
  void InitNoOpPeerProxies();
  void InitDelayableMockedProxies(bool enable_delay);
  gscoped_ptr<VoteCounter> InitVoteCounter(int num_voters, int majority_size);

  // Voter 0 is the high-term voter.
  scoped_refptr<LeaderElection> SetUpElectionWithHighTermVoter(ConsensusTerm election_term);

  // Predetermine the election results using the specified number of
  // grant / deny / error responses.
  // num_grant must be at least 1, for the candidate to vote for itself.
  // num_grant + num_deny + num_error must add up to an odd number.
  scoped_refptr<LeaderElection> SetUpElectionWithGrantDenyErrorVotes(ConsensusTerm election_term,
                                                                     int num_grant,
                                                                     int num_deny,
                                                                     int num_error);

  const string tablet_id_;
  string candidate_uuid_;
  vector<string> voter_uuids_;

  RaftConfigPB config_;
  ProxyMap proxies_;
  gscoped_ptr<PeerProxyFactory> proxy_factory_;
  gscoped_ptr<ThreadPool> pool_;

  CountDownLatch latch_;
  gscoped_ptr<ElectionResult> result_;
};

void LeaderElectionTest::ElectionCallback(const ElectionResult& result) {
  result_.reset(new ElectionResult(result));
  latch_.CountDown();
}

void LeaderElectionTest::InitUUIDs(int num_voters) {
  voter_uuids_ = GenVoterUUIDs(num_voters);
  CHECK(!voter_uuids_.empty());
  candidate_uuid_ = voter_uuids_.back();
  voter_uuids_.pop_back();
}

void LeaderElectionTest::InitNoOpPeerProxies() {
  config_.Clear();
  for (const string& uuid : voter_uuids_) {
    RaftPeerPB* peer_pb = config_.add_peers();
    peer_pb->set_permanent_uuid(uuid);
    peer_pb->set_member_type(RaftPeerPB::VOTER);
    PeerProxy* proxy = new NoOpTestPeerProxy(pool_.get(), *peer_pb);
    InsertOrDie(&proxies_, uuid, proxy);
  }
}

void LeaderElectionTest::InitDelayableMockedProxies(bool enable_delay) {
  config_.Clear();
  for (const string& uuid : voter_uuids_) {
    RaftPeerPB* peer_pb = config_.add_peers();
    peer_pb->set_permanent_uuid(uuid);
    peer_pb->set_member_type(RaftPeerPB::VOTER);
    auto proxy = new DelayablePeerProxy<MockedPeerProxy>(pool_.get(),
                                                         new MockedPeerProxy(pool_.get()));
    if (enable_delay) {
      proxy->DelayResponse();
    }
    InsertOrDie(&proxies_, uuid, proxy);
  }
}

gscoped_ptr<VoteCounter> LeaderElectionTest::InitVoteCounter(int num_voters, int majority_size) {
  gscoped_ptr<VoteCounter> counter(new VoteCounter(num_voters, majority_size));
  bool duplicate;
  CHECK_OK(counter->RegisterVote(candidate_uuid_, VOTE_GRANTED, &duplicate));
  CHECK(!duplicate);
  return std::move(counter);
}

scoped_refptr<LeaderElection> LeaderElectionTest::SetUpElectionWithHighTermVoter(
    ConsensusTerm election_term) {
  const int kNumVoters = 3;
  const int kMajoritySize = 2;

  InitUUIDs(kNumVoters);
  InitDelayableMockedProxies(true);
  gscoped_ptr<VoteCounter> counter = InitVoteCounter(kNumVoters, kMajoritySize);

  VoteResponsePB response;
  response.set_responder_uuid(voter_uuids_[0]);
  response.set_responder_term(election_term + 1);
  response.set_vote_granted(false);
  response.mutable_consensus_error()->set_code(ConsensusErrorPB::INVALID_TERM);
  StatusToPB(Status::InvalidArgument("Bad term"),
      response.mutable_consensus_error()->mutable_status());
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[0]])
      ->proxy()->set_vote_response(response);

  response.Clear();
  response.set_responder_uuid(voter_uuids_[1]);
  response.set_responder_term(election_term);
  response.set_vote_granted(true);
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[1]])
      ->proxy()->set_vote_response(response);

  VoteRequestPB request;
  request.set_candidate_uuid(candidate_uuid_);
  request.set_candidate_term(election_term);
  request.set_tablet_id(tablet_id_);

  scoped_refptr<LeaderElection> election(
      new LeaderElection(config_, proxy_factory_.get(),
                         std::move(request), std::move(counter),
                         MonoDelta::FromSeconds(kLeaderElectionTimeoutSecs),
                         std::bind(&LeaderElectionTest::ElectionCallback,
                                   this,
                                   std::placeholders::_1)));
  return election;
}

scoped_refptr<LeaderElection> LeaderElectionTest::SetUpElectionWithGrantDenyErrorVotes(
    ConsensusTerm election_term, int num_grant, int num_deny, int num_error) {
  const int kNumVoters = num_grant + num_deny + num_error;
  CHECK_GE(num_grant, 1);       // Gotta vote for yourself.
  CHECK_EQ(1, kNumVoters % 2);  // RaftConfig size must be odd.
  const int kMajoritySize = (kNumVoters / 2) + 1;

  InitUUIDs(kNumVoters);
  InitDelayableMockedProxies(false); // Don't delay the vote responses.
  gscoped_ptr<VoteCounter> counter = InitVoteCounter(kNumVoters, kMajoritySize);
  int num_grant_followers = num_grant - 1;

  // Set up mocked responses based on the params specified in the method arguments.
  int voter_index = 0;
  while (voter_index < voter_uuids_.size()) {
    VoteResponsePB response;
    if (num_grant_followers > 0) {
      response.set_responder_uuid(voter_uuids_[voter_index]);
      response.set_responder_term(election_term);
      response.set_vote_granted(true);
      --num_grant_followers;
    } else if (num_deny > 0) {
      response.set_responder_uuid(voter_uuids_[voter_index]);
      response.set_responder_term(election_term);
      response.set_vote_granted(false);
      response.mutable_consensus_error()->set_code(ConsensusErrorPB::LAST_OPID_TOO_OLD);
      StatusToPB(Status::InvalidArgument("Last OpId"),
          response.mutable_consensus_error()->mutable_status());
      --num_deny;
    } else if (num_error > 0) {
      response.mutable_error()->set_code(tserver::TabletServerErrorPB::TABLET_NOT_FOUND);
      StatusToPB(Status::NotFound("Unknown Tablet"),
          response.mutable_error()->mutable_status());
      --num_error;
    } else {
      LOG(FATAL) << "Unexpected fallthrough";
    }

    down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[voter_index]])
        ->proxy()->set_vote_response(response);
    ++voter_index;
  }

  VoteRequestPB request;
  request.set_candidate_uuid(candidate_uuid_);
  request.set_candidate_term(election_term);
  request.set_tablet_id(tablet_id_);

  scoped_refptr<LeaderElection> election(
      new LeaderElection(config_, proxy_factory_.get(),
                         std::move(request), std::move(counter),
                         MonoDelta::FromSeconds(kLeaderElectionTimeoutSecs),
                         std::bind(&LeaderElectionTest::ElectionCallback,
                                   this,
                                   std::placeholders::_1)));
  return election;
}

// All peers respond "yes", no failures.
TEST_F(LeaderElectionTest, TestPerfectElection) {
  // Try configuration sizes of 1, 3, 5.
  vector<int> config_sizes = { 1, 3, 5 };
  for (int num_voters : config_sizes) {
    LOG(INFO) << "Testing election with config size of " << num_voters;
    int majority_size = (num_voters / 2) + 1;
    ConsensusTerm election_term = 10L + num_voters; // Just to be able to differentiate.

    InitUUIDs(num_voters);
    InitNoOpPeerProxies();
    gscoped_ptr<VoteCounter> counter = InitVoteCounter(num_voters, majority_size);

    VoteRequestPB request;
    request.set_candidate_uuid(candidate_uuid_);
    request.set_candidate_term(election_term);
    request.set_tablet_id(tablet_id_);

    scoped_refptr<LeaderElection> election(
        new LeaderElection(config_, proxy_factory_.get(),
                           std::move(request), std::move(counter),
                           MonoDelta::FromSeconds(kLeaderElectionTimeoutSecs),
                           std::bind(&LeaderElectionTest::ElectionCallback,
                                     this,
                                     std::placeholders::_1)));
    election->Run();
    latch_.Wait();

    ASSERT_EQ(election_term, result_->vote_request.candidate_term());
    ASSERT_EQ(VOTE_GRANTED, result_->decision);

    pool_->Wait();
    proxies_.clear(); // We don't delete them; The election VoterState object
                      // ends up owning them.
    latch_.Reset(1);
  }
}

// Test leader election when we encounter a peer with a higher term before we
// have arrived at a majority decision.
TEST_F(LeaderElectionTest, TestHigherTermBeforeDecision) {
  const ConsensusTerm kElectionTerm = 2;
  scoped_refptr<LeaderElection> election = SetUpElectionWithHighTermVoter(kElectionTerm);
  election->Run();

  // This guy has a higher term.
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[0]])
      ->Respond(TestPeerProxy::kRequestVote);
  latch_.Wait();

  ASSERT_EQ(kElectionTerm, result_->vote_request.candidate_term());
  ASSERT_EQ(VOTE_DENIED, result_->decision);
  ASSERT_EQ(kElectionTerm + 1, result_->highest_voter_term);
  LOG(INFO) << "Election lost. Reason: " << result_->message;

  // This guy will vote "yes".
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[1]])
      ->Respond(TestPeerProxy::kRequestVote);

  pool_->Wait(); // Wait for the election callbacks to finish before we destroy proxies.
}

// Test leader election when we encounter a peer with a higher term after we
// have arrived at a majority decision of "yes".
TEST_F(LeaderElectionTest, TestHigherTermAfterDecision) {
  const ConsensusTerm kElectionTerm = 2;
  scoped_refptr<LeaderElection> election = SetUpElectionWithHighTermVoter(kElectionTerm);
  election->Run();

  // This guy will vote "yes".
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[1]])
      ->Respond(TestPeerProxy::kRequestVote);
  latch_.Wait();

  ASSERT_EQ(kElectionTerm, result_->vote_request.candidate_term());
  ASSERT_EQ(VOTE_GRANTED, result_->decision);
  ASSERT_EQ(kElectionTerm, result_->highest_voter_term);
  ASSERT_EQ("achieved majority votes", result_->message);
  LOG(INFO) << "Election won.";

  // This guy has a higher term.
  down_cast<DelayablePeerProxy<MockedPeerProxy>*>(proxies_[voter_uuids_[0]])
      ->Respond(TestPeerProxy::kRequestVote);

  pool_->Wait(); // Wait for the election callbacks to finish before we destroy proxies.
}

// Out-of-date OpId "vote denied" case.
TEST_F(LeaderElectionTest, TestWithDenyVotes) {
  const ConsensusTerm kElectionTerm = 2;
  const int kNumGrant = 2;
  const int kNumDeny = 3;
  const int kNumError = 0;
  scoped_refptr<LeaderElection> election =
      SetUpElectionWithGrantDenyErrorVotes(kElectionTerm, kNumGrant, kNumDeny, kNumError);
  LOG(INFO) << "Running";
  election->Run();

  latch_.Wait();
  ASSERT_EQ(kElectionTerm, result_->vote_request.candidate_term());
  ASSERT_EQ(VOTE_DENIED, result_->decision);
  ASSERT_EQ(kElectionTerm, result_->highest_voter_term);
  ASSERT_EQ("could not achieve majority", result_->message);
  LOG(INFO) << "Election denied.";

  pool_->Wait(); // Wait for the election callbacks to finish before we destroy proxies.
}

// Count errors as denied votes.
TEST_F(LeaderElectionTest, TestWithErrorVotes) {
  const ConsensusTerm kElectionTerm = 2;
  const int kNumGrant = 1;
  const int kNumDeny = 0;
  const int kNumError = 4;
  scoped_refptr<LeaderElection> election =
      SetUpElectionWithGrantDenyErrorVotes(kElectionTerm, kNumGrant, kNumDeny, kNumError);
  election->Run();

  latch_.Wait();
  ASSERT_EQ(kElectionTerm, result_->vote_request.candidate_term());
  ASSERT_EQ(VOTE_DENIED, result_->decision);
  ASSERT_EQ(0, result_->highest_voter_term); // no valid votes
  ASSERT_EQ("could not achieve majority", result_->message);
  LOG(INFO) << "Election denied.";

  pool_->Wait(); // Wait for the election callbacks to finish before we destroy proxies.
}

// Count errors as denied votes.
TEST_F(LeaderElectionTest, TestFailToCreateProxy) {
  const ConsensusTerm kElectionTerm = 2;
  const int kNumVoters = 3;
  const int kMajoritySize = 2;

  // Initialize the UUIDs and the proxies (which also sets up the config PB).
  InitUUIDs(kNumVoters);
  InitNoOpPeerProxies();

  // Remove all the proxies. This will make our peer factory return a bad Status.
  STLDeleteValues(&proxies_);

  // Our election should now fail as if the votes were denied.
  VoteRequestPB request;
  request.set_candidate_uuid(candidate_uuid_);
  request.set_candidate_term(kElectionTerm);
  request.set_tablet_id(tablet_id_);

  gscoped_ptr<VoteCounter> counter = InitVoteCounter(kNumVoters, kMajoritySize);
  scoped_refptr<LeaderElection> election(
      new LeaderElection(config_, proxy_factory_.get(),
                         std::move(request), std::move(counter),
                         MonoDelta::FromSeconds(kLeaderElectionTimeoutSecs),
                         std::bind(&LeaderElectionTest::ElectionCallback,
                                   this,
                                   std::placeholders::_1)));
  election->Run();
  latch_.Wait();
  ASSERT_EQ(kElectionTerm, result_->vote_request.candidate_term());
  ASSERT_EQ(VOTE_DENIED, result_->decision);
  ASSERT_EQ(0, result_->highest_voter_term); // no votes
  ASSERT_EQ("could not achieve majority", result_->message);
}

////////////////////////////////////////
// VoteCounterTest
////////////////////////////////////////

class VoteCounterTest : public KuduTest {
 protected:
  static void AssertUndecided(const VoteCounter& counter);
  static void AssertVoteCount(const VoteCounter& counter, int yes_votes, int no_votes);
};

void VoteCounterTest::AssertUndecided(const VoteCounter& counter) {
  ASSERT_FALSE(counter.IsDecided());
  ElectionVote decision;
  Status s = counter.GetDecision(&decision);
  ASSERT_TRUE(s.IsIllegalState());
  ASSERT_STR_CONTAINS(s.ToString(), "Vote not yet decided");
}

void VoteCounterTest::AssertVoteCount(const VoteCounter& counter, int yes_votes, int no_votes) {
  ASSERT_EQ(yes_votes, counter.yes_votes_);
  ASSERT_EQ(no_votes, counter.no_votes_);
  ASSERT_EQ(yes_votes + no_votes, counter.GetTotalVotesCounted());
}

// Test basic vote counting functionality with an early majority.
TEST_F(VoteCounterTest, TestVoteCounter_EarlyDecision) {
  const int kNumVoters = 3;
  const int kMajoritySize = 2;
  vector<string> voter_uuids = GenVoterUUIDs(kNumVoters);

  // "Yes" decision.
  {
    // Start off undecided.
    VoteCounter counter(kNumVoters, kMajoritySize);
    ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 0, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // First yes vote.
    bool duplicate;
    ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_GRANTED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 1, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // Second yes vote wins it in a configuration of 3.
    ASSERT_OK(counter.RegisterVote(voter_uuids[1], VOTE_GRANTED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_TRUE(counter.IsDecided());
    ElectionVote decision;
    ASSERT_OK(counter.GetDecision(&decision));
    ASSERT_TRUE(decision == VOTE_GRANTED);
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 2, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());
  }

  // "No" decision.
  {
    // Start off undecided.
    VoteCounter counter(kNumVoters, kMajoritySize);
    ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 0, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // First no vote.
    bool duplicate;
    ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_DENIED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 0, 1));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // Second no vote loses it in a configuration of 3.
    ASSERT_OK(counter.RegisterVote(voter_uuids[1], VOTE_DENIED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_TRUE(counter.IsDecided());
    ElectionVote decision;
    ASSERT_OK(counter.GetDecision(&decision));
    ASSERT_TRUE(decision == VOTE_DENIED);
    ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 0, 2));
    ASSERT_FALSE(counter.AreAllVotesIn());
  }
}

// Test basic vote counting functionality with the last vote being the deciding vote.
TEST_F(VoteCounterTest, TestVoteCounter_LateDecision) {
  const int kNumVoters = 5;
  const int kMajoritySize = 3;
  vector<string> voter_uuids = GenVoterUUIDs(kNumVoters);

  // Start off undecided.
  VoteCounter counter(kNumVoters, kMajoritySize);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 0, 0));
  ASSERT_FALSE(counter.AreAllVotesIn());

  // Add single yes vote, still undecided.
  bool duplicate;
  ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_GRANTED, &duplicate));
  ASSERT_FALSE(duplicate);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 1, 0));
  ASSERT_FALSE(counter.AreAllVotesIn());

  // Attempt duplicate vote.
  ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_GRANTED, &duplicate));
  ASSERT_TRUE(duplicate);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 1, 0));
  ASSERT_FALSE(counter.AreAllVotesIn());

  // Attempt to change vote.
  Status s = counter.RegisterVote(voter_uuids[0], VOTE_DENIED, &duplicate);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), "voted a different way twice");
  LOG(INFO) << "Expected vote-changed error: " << s.ToString();
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 1, 0));
  ASSERT_FALSE(counter.AreAllVotesIn());

  // Add more votes...
  ASSERT_OK(counter.RegisterVote(voter_uuids[1], VOTE_DENIED, &duplicate));
  ASSERT_FALSE(duplicate);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 1, 1));
  ASSERT_FALSE(counter.AreAllVotesIn());

  ASSERT_OK(counter.RegisterVote(voter_uuids[2], VOTE_GRANTED, &duplicate));
  ASSERT_FALSE(duplicate);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 2, 1));
  ASSERT_FALSE(counter.AreAllVotesIn());

  ASSERT_OK(counter.RegisterVote(voter_uuids[3], VOTE_DENIED, &duplicate));
  ASSERT_FALSE(duplicate);
  ASSERT_NO_FATAL_FAILURE(AssertUndecided(counter));
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 2, 2));
  ASSERT_FALSE(counter.AreAllVotesIn());

  // Win the election.
  ASSERT_OK(counter.RegisterVote(voter_uuids[4], VOTE_GRANTED, &duplicate));
  ASSERT_FALSE(duplicate);
  ASSERT_TRUE(counter.IsDecided());
  ElectionVote decision;
  ASSERT_OK(counter.GetDecision(&decision));
  ASSERT_TRUE(decision == VOTE_GRANTED);
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 3, 2));
  ASSERT_TRUE(counter.AreAllVotesIn());

  // Attempt to vote with > the whole configuration.
  s = counter.RegisterVote("some-random-node", VOTE_GRANTED, &duplicate);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), "cause the number of votes to exceed the expected number");
  LOG(INFO) << "Expected voters-exceeded error: " << s.ToString();
  ASSERT_TRUE(counter.IsDecided());
  ASSERT_NO_FATAL_FAILURE(AssertVoteCount(counter, 3, 2));
  ASSERT_TRUE(counter.AreAllVotesIn());
}

// Test vote counting with an even number of voters.
TEST_F(VoteCounterTest, TestVoteCounter_EvenVoters) {
  const int kNumVoters = 2;
  const int kMajoritySize = 2;
  vector<string> voter_uuids = GenVoterUUIDs(kNumVoters);

  // "Yes" decision.
  {
    VoteCounter counter(kNumVoters, kMajoritySize);
    NO_FATALS(AssertUndecided(counter));
    NO_FATALS(AssertVoteCount(counter, 0, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // Initial yes vote.
    bool duplicate;
    ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_GRANTED, &duplicate));
    ASSERT_FALSE(duplicate);
    NO_FATALS(AssertUndecided(counter));
    NO_FATALS(AssertVoteCount(counter, 1, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // Second yes vote wins it.
    ASSERT_OK(counter.RegisterVote(voter_uuids[1], VOTE_GRANTED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_TRUE(counter.IsDecided());
    ElectionVote decision;
    ASSERT_OK(counter.GetDecision(&decision));
    ASSERT_TRUE(decision == VOTE_GRANTED);
    NO_FATALS(AssertVoteCount(counter, 2, 0));
    ASSERT_TRUE(counter.AreAllVotesIn());
  }

  // "No" decision.
  {
    VoteCounter counter(kNumVoters, kMajoritySize);
    NO_FATALS(AssertUndecided(counter));
    NO_FATALS(AssertVoteCount(counter, 0, 0));
    ASSERT_FALSE(counter.AreAllVotesIn());

    // The first "no" vote guarantees a failed election when num voters == 2.
    bool duplicate;
    ASSERT_OK(counter.RegisterVote(voter_uuids[0], VOTE_DENIED, &duplicate));
    ASSERT_FALSE(duplicate);
    ASSERT_TRUE(counter.IsDecided());
    ElectionVote decision;
    ASSERT_OK(counter.GetDecision(&decision));
    ASSERT_TRUE(decision == VOTE_DENIED);
    NO_FATALS(AssertVoteCount(counter, 0, 1));
    ASSERT_FALSE(counter.AreAllVotesIn());
  }
}


}  // namespace consensus
}  // namespace kudu
