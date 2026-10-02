#pragma once

#include <vector>
#include <cstddef>
#include <iterator>
#include <algorithm>

template <typename T>
class RingBuffer {
private:
    std::vector<T> data_;
    std::size_t capacity_ = 60;
    std::size_t head_ = 0; // Index of the oldest element when buffer is full
    std::size_t size_ = 0; // Current number of elements

public:
    explicit RingBuffer(std::size_t capacity = 60) 
        : data_(capacity), capacity_(capacity), head_(0), size_(0) {
        if (capacity == 0) {
            capacity_ = 1;
            data_.resize(1);
        }
    }

    void push_back(const T& val) {
        if (size_ < capacity_) {
            data_[size_] = val;
            size_++;
        } else {
            data_[head_] = val;
            head_ = (head_ + 1) % capacity_;
        }
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    const T& operator[](std::size_t i) const {
        if (size_ < capacity_) {
            return data_[i];
        } else {
            return data_[(head_ + i) % capacity_];
        }
    }

    T& operator[](std::size_t i) {
        if (size_ < capacity_) {
            return data_[i];
        } else {
            return data_[(head_ + i) % capacity_];
        }
    }

    class const_iterator {
    private:
        const RingBuffer* buffer_;
        std::size_t index_;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator(const RingBuffer* buf, std::size_t idx) : buffer_(buf), index_(idx) {}

        reference operator*() const { return (*buffer_)[index_]; }
        pointer operator->() const { return &((*buffer_)[index_]); }

        const_iterator& operator++() {
            index_++;
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator tmp = *this;
            index_++;
            return tmp;
        }

        bool operator==(const const_iterator& other) const {
            return buffer_ == other.buffer_ && index_ == other.index_;
        }

        bool operator!=(const const_iterator& other) const {
            return !(*this == other);
        }
    };

    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }
};
