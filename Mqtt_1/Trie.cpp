#include "Trie.h"


	Trie::Trie() {
		root_ = new TrieNode();
	}
	
	int Trie::subscribe(const std::string& topic, const std::string& client_id) {
		TrieNode* current = root_;
		size_t pos = 0;
		while (pos < topic.size()) {
			size_t next_pos = topic.find('/', pos);
			std::string part = topic.substr(pos, next_pos - pos);
			if (current->children.find(part) == current->children.end()) {
				current->children[part] = new TrieNode();
			}
			current = current->children[part];
			pos = (next_pos == std::string::npos) ? topic.size() : next_pos + 1;
		}
		current->client_ids.insert(client_id);
		return 0; // Success
	}

	int Trie::add_client_id(const std::string& topic, const std::string& client_id) {
		return subscribe(topic, client_id);
	}

	int Trie::get_children(const std::string& topic, std::set<std::string>& client_ids) {
		return get_children(topic, client_ids, root_);
	}

	

	int Trie::match_topic(const std::string& topic, std::set<std::string>& client_ids) {
		match_recursive(topic, 0, root_, client_ids);
		return 0;
	}

	int Trie::unsubscribe(const std::string& topic, const std::string& client_id) {
		TrieNode* current = root_;
		size_t pos = 0;
		while (pos < topic.size()) {
			size_t next_pos = topic.find('/', pos);
			std::string part = topic.substr(pos, next_pos - pos);
			if (current->children.find(part) == current->children.end()) {
				return -1; // Not found
			}
			current = current->children[part];
			pos = (next_pos == std::string::npos) ? topic.size() : next_pos + 1;
		}
		current->client_ids.erase(client_id);
		return 0; // Success
	}

	int Trie::remove_client_id(const std::string& topic, const std::string& client_id) {
		return unsubscribe(topic, client_id);
	}

	Trie::~Trie() {
		destroy(root_);
	}

	int Trie::get_children(const std::string& topic, std::set<std::string>& client_ids, TrieNode* node) {
		if (!node) return -1; // Not found
		size_t pos = 0;
		while (pos < topic.size()) {
			size_t next_pos = topic.find('/', pos);
			std::string part = topic.substr(pos, next_pos - pos);
			if (node->children.find(part) == node->children.end()) {
				return -1; // Not found
			}
			node = node->children[part];
			pos = (next_pos == std::string::npos) ? topic.size() : next_pos + 1;
		}
		client_ids.insert(node->client_ids.begin(), node->client_ids.end());
		return 0; // Success
	}

	void Trie::destroy(TrieNode* node) {
		if (!node)  return;

		for (auto& [key, child] : node->children) {
			destroy(child);
		}

		delete node;
	}

	void Trie::match_recursive(const std::string& topic, size_t pos, TrieNode* node,
		std::set<std::string>& client_ids) {
		if (!node) return;

		// '#' matches everything remaining — collect and stop
		auto it_hash = node->children.find("#");
		if (it_hash != node->children.end()) {
			auto& ids = it_hash->second->client_ids;
			client_ids.insert(ids.begin(), ids.end());
		}

		// End of topic — collect exact matches
		if (pos == topic.size()) {
			client_ids.insert(node->client_ids.begin(), node->client_ids.end());
			return;
		}

		size_t next_pos = topic.find('/', pos);
		std::string part = topic.substr(pos, next_pos == std::string::npos
			? std::string::npos : next_pos - pos);
		size_t new_pos = (next_pos == std::string::npos) ? topic.size() : next_pos + 1;

		// '+' matches exactly one level
		auto it_plus = node->children.find("+");
		if (it_plus != node->children.end())
			match_recursive(topic, new_pos, it_plus->second, client_ids);

		// Exact segment match
		auto it_exact = node->children.find(part);
		if (it_exact != node->children.end())
			match_recursive(topic, new_pos, it_exact->second, client_ids);
	}