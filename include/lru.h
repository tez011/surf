#pragma once
#include <list>
#include <map>

template<typename T>
class lru {
private:
	typedef typename std::list<T>::iterator list_iterator_t;
	std::list<T> l;
	std::map<T, list_iterator_t> m;
	size_t max_size;
public:

	lru(size_t sz) : max_size(sz)
	{
	}

	inline size_t size() const
	{
		return m.size();
	}

	inline size_t get_max_size() const {
		return max_size;
	}

	void put(const T& value)
	{
		auto it = m.find(value);
		l.push_front(value);
		if (it != m.end()) {
			l.erase(it->second);
			m.erase(it);
		}
		m[value] = l.begin();

		if (m.size() > max_size) {
			auto last = l.end();
			last--;
			evict(*last);
			m.erase(*last);
			l.pop_back();
		}
	}

	inline bool contains(const T& value) const
	{
		return m.find(value) != m.end();
	}

	virtual void evict(const T& evicted_value) = 0;
};
