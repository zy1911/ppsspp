#pragma once

#include "ext/xxhash.h"
#include <functional>

// Whatever random value.
const uint32_t hashmapSeed = 0x23B58532;

// TODO: Try hardware CRC. Unfortunately not available on older Intels or ARM32.
// Seems to be ubiquitous on ARM64 though.
template<class K>
inline uint32_t HashKey(const K &k) {
	return XXH32(&k, sizeof(k), hashmapSeed);
}
template<class K>
inline bool KeyEquals(const K &a, const K &b) {
	return !memcmp(&a, &b, sizeof(K));
}

enum class BucketState {
	FREE,
	TAKEN,
	REMOVED,  // for linear probing to work we need tombstones
};


// Uses linear probing for cache-friendliness. Not segregating values from keys because
// we always use very small values, so it's probably better to have them in the same
// cache-line as the corresponding key.
// Enforces that value are pointers to make sure that combined storage makes sense.
template <class Key, class Value>
class DenseHashMap {
public:
	DenseHashMap(int initialCapacity) : capacity_(initialCapacity) {
		map.resize(initialCapacity);
	}

	// Returns nullptr if no entry was found.
	Value Get(const Key &key) {
		uint32_t mask = capacity_ - 1;
		uint32_t pos = HashKey(key) & mask;
		// No? Let's go into search mode. Linear probing.
		uint32_t p = pos;
		while (true) {
			if (map[p].state == BucketState::TAKEN && KeyEquals(key, map[p].key))
				return map[p].value;
			else if (map[p].state == BucketState::FREE)
				return nullptr;
			p = (p + 1) & mask;  // If the state is REMOVED, we just keep on walking. 
			if (p == pos)
				DebugBreak();
		}
		return nullptr;
	}

	// Returns false if we already had the key! Which is a bit different.
	bool Insert(const Key &key, Value value) {
		// Check load factor, resize if necessary. We never shrink.
		if (count_ > capacity_ / 2) {
			Grow();
		}
		uint32_t mask = capacity_ - 1;
		uint32_t pos = HashKey(key) & mask;
		uint32_t p = pos;
		while (true) {
			if (map[p].state == BucketState::TAKEN) {
				if (KeyEquals(key, map[p].key)) {
					DebugBreak();  // Bad! We already got this one. Let's avoid this case.
					return false;
				}
				// continue looking....
			} else {
				// Got a place, either removed or FREE.
				break;
			}
			p = (p + 1) & mask;
			if (p == pos) {
				// FULL! Error. Should not happen thanks to Grow().
				DebugBreak();
			}
		}
		map[p].state = BucketState::TAKEN;
		map[p].key = key;
		map[p].value = value;
		count_++;
		return true;
	}

	void Remove(const Key &key) {
		uint32_t mask = capacity_ - 1;
		uint32_t pos = HashKey(key) & mask;
		uint32_t p = pos;
		while (map[p].state != BucketState::FREE) {
			if (map[p].state == BucketState::TAKEN && KeyEquals(key, map[p].key)) {
				// Got it! Mark it as removed.
				map[p].state = BucketState::REMOVED;
				count_--;
				return;
			}
			p = (p + 1) & mask;
			if (p == pos) {
				// FULL! Error. Should not happen.
				DebugBreak();
			}
		}
	}

	size_t size() const {
		return count_;
	}

	// TODO: Find a way to avoid std::function. I tried using a templated argument
	// but couldn't get it to pass the compiler.
	inline void Iterate(std::function<void(const typename Key &key, typename Value value)> func) {
		for (auto &iter : map) {
			if (iter.state == BucketState::TAKEN) {
				func(iter.key, iter.value);
			}
		}
	}

	void Clear() {
		// TODO: Speedup?
		map.clear();
		map.resize(capacity_);
	}

private:
	void Grow() {
		// We simply move out the existing data, then we re-insert the old.
		// This is extremely non-atomic and will need synchronization.
		std::vector<Pair> old = std::move(map);
		capacity_ *= 2;
		map.clear();
		map.resize(capacity_);
		count_ = 0;  // Insert will update it.
		for (auto &iter : old) {
			if (iter.state == BucketState::TAKEN) {
				Insert(iter.key, iter.value);
			}
		}
	}
	struct Pair {
		BucketState state;
		Key key;
		Value value;
	};
	std::vector<Pair> map;
	int capacity_;
	int count_ = 0;
};

// Like the above, uses linear probing for cache-friendliness.
// Does not perform hashing at all so expects well-distributed keys.
template <class Value>
class PrehashMap {
public:
	PrehashMap(int initialCapacity) : capacity_(initialCapacity) {
		map.resize(initialCapacity);
	}

	// Returns nullptr if no entry was found.
	Value Get(uint32_t hash) {
		uint32_t mask = capacity_ - 1;
		uint32_t pos = hash & mask;
		// No? Let's go into search mode. Linear probing.
		uint32_t p = pos;
		while (true) {
			if (map[p].state == BucketState::TAKEN && hash == map[p].hash)
				return map[p].value;
			else if (map[p].state == BucketState::FREE)
				return nullptr;
			p = (p + 1) & mask;  // If the state is REMOVED, we just keep on walking. 
			if (p == pos)
				DebugBreak();
		}
		return nullptr;
	}

	// Returns false if we already had the key! Which is a bit different.
	bool Insert(uint32_t hash, Value value) {
		// Check load factor, resize if necessary. We never shrink.
		if (count_ > capacity_ / 2) {
			Grow();
		}
		uint32_t mask = capacity_ - 1;
		uint32_t pos = hash & mask;
		uint32_t p = pos;
		while (map[p].state != BucketState::FREE) {
			if (map[p].state == BucketState::TAKEN) {
				if (hash == map[p].hash)
					return false;  // Bad!
			} else {
				// Got a place, either removed or FREE.
				break;
			}
			p = (p + 1) & mask;
			if (p == pos) {
				// FULL! Error. Should not happen thanks to Grow().
				DebugBreak();
			}
		}
		map[p].state = BucketState::TAKEN;
		map[p].hash = hash;
		map[p].value = value;
		count_++;
		return true;
	}

	void Remove(uint32_t hash) {
		uint32_t mask = capacity_ - 1;
		uint32_t pos = hash & mask;
		uint32_t p = pos;
		while (map[p].state != BucketState::FREE) {
			if (map[p].state == BucketState::TAKEN && hash == map[p].hash) {
				// Got it!
				map[p].state = BucketState::REMOVED;
				count_--;
				return;
			}
			p = (p + 1) & mask;
			if (p == pos) {
				// FULL! Error. Should not happen.
				DebugBreak();
			}
		}
	}

	size_t size() {
		return count_;
	}

	// TODO: Find a way to avoid std::function. I tried using a templated argument
	// but couldn't get it to pass the compiler.
	void Iterate(std::function<void(uint32_t hash, typename Value value)> func) {
		for (auto &iter : map) {
			if (iter.state == BucketState::TAKEN) {
				func(iter.hash, iter.value);
			}
		}
	}

	void Clear() {
		// TODO: Speedup?
		map.clear();
		map.resize(capacity_);
	}

private:
	void Grow() {
		// We simply move out the existing data, then we re-insert the old.
		// This is extremely non-atomic and will need synchronization.
		std::vector<Pair> old = std::move(map);
		capacity_ *= 2;
		map.clear();
		map.resize(capacity_);
		for (auto &iter : old) {
			if (iter.state == BucketState::TAKEN) {
				Insert(iter.hash, iter.value);
			}
		}
	}
	struct Pair {
		BucketState state;
		uint32_t hash;
		Value value;
	};
	std::vector<Pair> map;
	int capacity_;
	int count_ = 0;
};
