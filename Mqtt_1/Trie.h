#pragma once
#include <unordered_map>
#include <string>
#include <set>

struct TrieNode {
	std::unordered_map<std::string, TrieNode*> children;
	std::set<std::string> client_ids;
};

class Trie
{
public:
	Trie();

	int subscribe(const std::string& topic, const std::string& client_id);
	int add_client_id(const std::string& topic, const std::string& client_id);

	int get_children(const std::string& topic, std::set<std::string>& client_ids);

	int match_topic(const std::string& topic, std::set<std::string>& client_ids);
	
	int unsubscribe(const std::string& topic, const std::string& client_id);
	int remove_client_id(const std::string& topic, const std::string& client_id);

	~Trie();
private:
	TrieNode* root_;

	int get_children(const std::string& topic, std::set<std::string>& client_ids, TrieNode* node);
	void destroy(TrieNode* node);
	void match_recursive(const std::string& topic, size_t pos, TrieNode* node,
		std::set<std::string>& client_ids);
};

