/*******************************************************************************
 *  Copyright 2012 maidsafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of maidsafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of maidsafe.net. *
 ******************************************************************************/

#include <vector>

#include "maidsafe/routing/tests/routing_network.h"

namespace args = std::placeholders;

namespace maidsafe {

namespace routing {

namespace test {

class TestNode : public GenericNode {
 public:
  explicit TestNode(bool client_mode = false)
      : GenericNode(client_mode),
        messages_() {
    functors_.message_received = [&](const int32_t &mesasge_type,
                                     const std::string &message,
                                     const NodeId &/*group_id*/,
                                     ReplyFunctor reply_functor) {
        LOG(kInfo) << id_ << " -- Received: type <" << mesasge_type
                   << "> message : " << message.substr(0, 10);
        std::lock_guard<std::mutex> guard(mutex_);
        messages_.push_back(std::make_pair(mesasge_type, message));
        reply_functor("Response to >:<" + message);
    };
    LOG(kVerbose) << "RoutingNode constructor";
  }

  TestNode(bool client_mode, const NodeInfoAndPrivateKey& node_info)
      : GenericNode(client_mode, node_info),
      messages_() {
    functors_.message_received = [&](const int32_t &mesasge_type,
                                     const std::string &message,
                                     const NodeId &/*group_id*/,
                                     ReplyFunctor reply_functor) {
      LOG(kInfo) << id_ << " -- Received: type <" << mesasge_type
                 << "> message : " << message.substr(0, 10);
      std::lock_guard<std::mutex> guard(mutex_);
      messages_.push_back(std::make_pair(mesasge_type, message));
      reply_functor("Response to " + message);
    };
    LOG(kVerbose) << "RoutingNode constructor";
  }

  virtual ~TestNode() {}
  size_t MessagesSize() const { return messages_.size(); }

 protected:
  std::vector<std::pair<int32_t, std::string> > messages_;
};

template <typename NodeType>
class RoutingNetworkTest : public GenericNetwork<NodeType> {
 public:
  RoutingNetworkTest(void) : GenericNetwork<NodeType>() {}

 protected:
  /** Send messages from each source to each destination */
  testing::AssertionResult Send(const size_t &messages) {
    NodeId  group_id;
    size_t message_id(0), client_size(0), non_client_size(0);
    std::set<size_t> received_ids;
    for (auto node : this->nodes_)
      (node->client_mode()) ? client_size++ : non_client_size++;

    LOG(kVerbose) << "Network node size: " << client_size << " : " << non_client_size;

    size_t messages_count(0),
        expected_messages(non_client_size * (non_client_size - 1 + client_size) * messages);
    std::mutex mutex;
    std::condition_variable cond_var;
    for (size_t index = 0; index < messages; ++index) {
      for (auto source_node : this->nodes_) {
        for (auto dest_node : this->nodes_) {
          auto callable = [&] (const int32_t& result, const std::vector<std::string> &message) {
              if (result != kSuccess)
                return;
              std::lock_guard<std::mutex> lock(mutex);
              messages_count++;
              std::string data_id(message.at(0).substr(message.at(0).find(">:<") + 3,
                  message.at(0).find("<:>") - 3 - message.at(0).find(">:<")));
              received_ids.insert(boost::lexical_cast<size_t>(data_id));
              LOG(kVerbose) << "ResponseHandler .... " << messages_count << " msg_id: "
                            << data_id;
              if (messages_count == expected_messages) {
                cond_var.notify_one();
                LOG(kVerbose) << "ResponseHandler .... DONE " << messages_count;
              }
            };
          if (source_node->node_id() != dest_node->node_id()) {
            std::string data(RandomAlphaNumericString((RandomUint32() % 255 + 1) * 2^10));
            {
              std::lock_guard<std::mutex> lock(mutex);
              data = boost::lexical_cast<std::string>(++message_id) + "<:>" + data;
            }
            source_node->Send(NodeId(dest_node->node_id()), group_id, data, 101, callable,
                boost::posix_time::seconds(10), ConnectType::kSingle);
          }
        }
      }
    }

    std::unique_lock<std::mutex> lock(mutex);
    bool result = cond_var.wait_for(lock, std::chrono::seconds(60),
        [&]()->bool {
          LOG(kInfo) << " message count " << messages_count << " expected "
                     << expected_messages << "\n";
          return messages_count == expected_messages;
        });
    EXPECT_TRUE(result);
    if (!result) {
      for (size_t id(1); id <= expected_messages; ++id) {
        auto iter = received_ids.find(id);
        if (iter == received_ids.end())
          LOG(kVerbose) << "missing id: " << id;
      }
      return testing::AssertionFailure() << "Send operarion timed out: "
                                         << expected_messages - messages_count
                                         << " failed to reply.";
    }
    return testing::AssertionSuccess();
  }

  testing::AssertionResult GroupSend(const NodeId &node_id, const size_t &messages) {
    NodeId  group_id;
    size_t messages_count(0), expected_messages(4 * messages);
    std::string data(RandomAlphaNumericString((RandomUint32() % 255 + 1) * 1024));

    std::mutex mutex;
    std::condition_variable cond_var;
    for (size_t index = 0; index < messages; ++index) {
      auto callable = [&] (const int32_t& result, const std::vector<std::string> /*message*/) {
          if (result != kSuccess)
            return;
          std::lock_guard<std::mutex> lock(mutex);
          messages_count++;
          LOG(kVerbose) << "ResponseHandler .... " << messages_count;
          if (messages_count == expected_messages) {
            cond_var.notify_one();
            LOG(kVerbose) << "ResponseHandler .... DONE " << messages_count;
          }
      };
      this->nodes_[0]->Send(node_id, group_id, data, 101, callable, boost::posix_time::seconds(10),
                      ConnectType::kGroup);
    }

    std::unique_lock<std::mutex> lock(mutex);
    bool result = cond_var.wait_for(lock, std::chrono::seconds(15),
        [&]()->bool {
          LOG(kInfo) << " message count " << messages_count << " expected "
                     << expected_messages << "\n";
          return messages_count == expected_messages;
        });
    EXPECT_TRUE(result);
    if (!result) {
      return testing::AssertionFailure() << "Send operarion timed out: "
                                         << expected_messages - messages_count
                                         << " failed to reply.";
    }
    return testing::AssertionSuccess();
  }
};

TYPED_TEST_CASE_P(RoutingNetworkTest);

TYPED_TEST_P(RoutingNetworkTest, FUNC_Send) {
  this->SetUpNetwork(kServerSize);
  EXPECT_TRUE(this->Send(1));
}

TYPED_TEST_P(RoutingNetworkTest, FUNC_ClientSend) {
  this->SetUpNetwork(kServerSize, kClientSize);
  EXPECT_TRUE(this->Send(1));
  Sleep(boost::posix_time::seconds(12));  // This sleep is required for un-responded requests
}

TYPED_TEST_P(RoutingNetworkTest, FUNC_SendMulti) {
  this->SetUpNetwork(kServerSize);
  EXPECT_TRUE(this->Send(40));
}

TYPED_TEST_P(RoutingNetworkTest, FUNC_ClientSendMulti) {
  this->SetUpNetwork(kServerSize, kClientSize);
  EXPECT_TRUE(this->Send(3));
  Sleep(boost::posix_time::seconds(21));  // This sleep is required for un-responded requests
}


TYPED_TEST_P(RoutingNetworkTest, DISABLED_FUNC_SendToGroup) {
  uint8_t size(0), message_count(1);
  this->SetUpNetwork(kServerSize);
  size_t last_index(this->nodes_.size() - 1);
  while (size++ < 3)
    this->AddNode(false, GenerateUniqueRandomId(this->nodes_[last_index]->node_id(), 10));
  NodeId dest_id(this->nodes_[last_index]->node_id());
  EXPECT_TRUE(this->GroupSend(dest_id, message_count));
  for (size_t index = last_index; index < this->nodes_.size(); ++index)
    EXPECT_EQ(this->nodes_[index]->MessagesSize(), message_count);
}

REGISTER_TYPED_TEST_CASE_P(RoutingNetworkTest, FUNC_Send, FUNC_ClientSend,
                           FUNC_SendMulti, FUNC_ClientSendMulti, DISABLED_FUNC_SendToGroup);
INSTANTIATE_TYPED_TEST_CASE_P(MAIDSAFE, RoutingNetworkTest, TestNode);

}  // namespace test

}  // namespace routing

}  // namespace maidsafe