// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

// JitHashTable implements a mapping from a Key type to a Value type,
// via a hash table.

// JitHashTable takes four template parameters:
//    Key, KeyFuncs, Value, Allocator and Behavior.
// We don't assume that Key has hash or equality functions specific names;
// rather, we assume that KeyFuncs has the following static methods
//    int GetHashCode(Key)
//    bool Equals(Key, Key)
// and use those. An instantiator can thus make a small "adaptor class"
// to invoke existing instance method hash and/or equality functions.
// If the implementor of a candidate Key class K understands this convention,
// these static methods can be implemented by K, so that K can be used
// as the actual arguments for the both Key and KeyFuncs template parameters.
//
// The "Behavior" parameter must provide the following static members:
//
// s_growth_factor_numerator
// s_growth_factor_denominator                  Factor to grow allocation (numerator/denominator).
//                                              Typically inherited from default traits (3/2)
//
// s_density_factor_numerator
// s_density_factor_denominator                 Maximum occupied density of table before growth
//                                              occurs (num/denom).  Typically inherited (3/4).
//
// s_minimum_allocation                         Minimum table allocation count (size on first growth.)  It is
//                                              probably preferable to call Reallocate on initialization rather
//                                              than override this from the default traits.
//
// NoMemory()                                   Called when the hash table is unable to grow due to potential
//                                              overflow or the lack of a sufficiently large prime.

class JitHashTableBehavior
{
public:
    static const unsigned s_growth_factor_numerator   = 3;
    static const unsigned s_growth_factor_denominator = 2;

    static const unsigned s_density_factor_numerator   = 3;
    static const unsigned s_density_factor_denominator = 4;

    static const unsigned s_minimum_allocation = 7;

    inline static void DECLSPEC_NORETURN NoMemory()
    {
        NOMEM();
    }
};

// Stores info about primes, including the magic number and shift amount needed
// to implement a divide without using the divide instruction
class JitPrimeInfo
{
public:
    constexpr JitPrimeInfo()
        : prime(0)
        , magic(0)
        , shift(0)
    {
    }
    constexpr JitPrimeInfo(unsigned p, unsigned m, unsigned s)
        : prime(p)
        , magic(m)
        , shift(s)
    {
    }
    unsigned prime;
    unsigned magic;
    unsigned shift;

    // Compute `numerator` / `prime` using magic division
    unsigned magicNumberDivide(unsigned numerator) const
    {
        uint64_t num     = numerator;
        uint64_t mag     = magic;
        uint64_t product = (num * mag) >> (32 + shift);
        return (unsigned)product;
    }

    // Compute `numerator` % `prime` using magic division
    unsigned magicNumberRem(unsigned numerator) const
    {
        unsigned div    = magicNumberDivide(numerator);
        unsigned result = numerator - (div * prime);
        assert(result == numerator % prime);
        return result;
    }
};

// Table of primes and their magic-number-divide constant.
// For more info see the book "Hacker's Delight" chapter 10.9 "Unsigned Division by Divisors >= 1"
// These were selected by looking for primes, each roughly twice as big as the next, having
// 32-bit magic numbers, (because the algorithm for using 33-bit magic numbers is slightly slower).

extern const JitPrimeInfo jitPrimeInfo[27];

// Hash table class definition
//
// Several iterators are defined that work with range-based `for`:
//   KeyIteration: yields just the hash table keys
//   ValueIteration: yields just the hash table values
//   KeyValueIteration: yields just the hash table <key,value> pairs
//
// For example:
//
//    for (const unsigned int lclNum : LclVarRefCounts::KeyIteration(&defsInBlock))
//
//    for (ValueNumToAssertsMap::Node* const iter : ValueNumToAssertsMap::KeyValueIteration(optValueNumToAsserts))
//    {
//        // use iter->GetKey(), iter->GetValue()
//    }
//
template <typename Key,
          typename KeyFuncs,
          typename Value,
          typename Allocator = CompAllocator,
          typename Behavior  = JitHashTableBehavior>
class JitHashTable
{
public:
    // The node type.
    // The only reason this class is public is to support the NodeIterator and KeyValueIteration. Only GetKey()
    // and GetValue() need to be public methods.
    class Node
    {
        friend class JitHashTable;

        Node* m_next; // Assume that the alignment requirement of Key and Value are no greater than Node*,
                      // so put m_next first to avoid unnecessary padding.
        Key   m_key;
        Value m_val;

        template <class... Args>
        Node(Node* next, Key k, Args&&... args)
            : m_next(next)
            , m_key(k)
            , m_val(std::forward<Args>(args)...)
        {
        }

        void* operator new(size_t sz, Allocator alloc)
        {
            return alloc.template allocate<unsigned char>(sz);
        }

        void operator delete(void* p, Allocator alloc)
        {
            alloc.deallocate(p);
        }

    public:
        Key GetKey() const
        {
            return m_key;
        }

        Value GetValue() const
        {
            return m_val;
        }
    };

    //------------------------------------------------------------------------
    // JitHashTable: Construct an empty JitHashTable object.
    //
    // Arguments:
    //    alloc - the allocator to be used by the new JitHashTable object
    //
    // Notes:
    //    JitHashTable always starts out empty, with no allocation overhead.
    //    Call Reallocate to prime with an initial size if desired.
    //
    JitHashTable(Allocator alloc)
        : m_alloc(alloc)
        , m_table(nullptr)
        , m_tableSizeInfo()
        , m_tableCount(0)
        , m_tableMax(0)
    {
#ifndef __GNUC__ // these crash GCC
        static_assert_no_msg(Behavior::s_growth_factor_numerator > Behavior::s_growth_factor_denominator);
        static_assert_no_msg(Behavior::s_density_factor_numerator < Behavior::s_density_factor_denominator);
#endif
    }

    //------------------------------------------------------------------------
    // ~JitHashTable: Destruct the JitHashTable object.
    //
    // Notes:
    //    Destructs all keys and values stored in the table and frees all
    //    owned memory.
    //
    ~JitHashTable()
    {
        RemoveAll();
    }

    //------------------------------------------------------------------------
    // Lookup: Get the value associated to the specified key, if any.
    //
    // Arguments:
    //    k    - the key
    //    pVal - pointer to a location used to store the associated value
    //
    // Return Value:
    //    `true` if the key exists, `false` otherwise
    //
    // Notes:
    //    If the key does not exist *pVal is not updated. pVal may be nullptr
    //    so this function can be used to simply check if the key exists.
    //
    bool Lookup(Key k, Value* pVal = nullptr) const
    {
        Node* pN = FindNode(k);

        if (pN != nullptr)
        {
            if (pVal != nullptr)
            {
                *pVal = pN->m_val;
            }
            return true;
        }
        else
        {
            return false;
        }
    }

    //------------------------------------------------------------------------
    // LookupPointer: Get a pointer to the value associated to the specified key.
    // if any.
    //
    // Arguments:
    //    k - the key
    //
    // Return Value:
    //    A pointer to the value associated with the specified key or nullptr
    //    if the key is not found
    //
    // Notes:
    //    This is similar to `Lookup` but avoids copying the value and allows
    //    updating the value without using `Set`.
    //
    Value* LookupPointer(Key k) const
    {
        Node* pN = FindNode(k);

        if (pN != nullptr)
        {
            return &(pN->m_val);
        }
        else
        {
            return nullptr;
        }
    }

    //------------------------------------------------------------------------
    // LookupPointerOrAdd: Get a pointer to the value associated to the specified key.
    // If not present, add it with the specified default value and return a pointer to it.
    //
    // Arguments:
    //    k - the key
    //    defaultValue - Default value to add to the table if the key was not present
    //
    // Return Value:
    //    A pointer to the value associated with the specified key.
    //
    Value* LookupPointerOrAdd(Key k, Value defaultValue)
    {
        CheckGrowth();

        assert(m_tableSizeInfo.prime != 0);

        unsigned index = GetIndexForKey(k);

        Node* n = m_table[index];
        while (n != nullptr)
        {
            if (KeyFuncs::Equals(k, n->m_key))
            {
                return &n->m_val;
            }

            n = n->m_next;
        }

        n              = new (m_alloc) Node(m_table[index], k, defaultValue);
        m_table[index] = n;
        m_tableCount++;
        return &n->m_val;
    }

    enum SetKind
    {
        None,
        Overwrite
    };

    //------------------------------------------------------------------------
    // Set: Associate the specified value with the specified key.
    //
    // Arguments:
    //    k - the key
    //    v - the value
    //    kind - None, we are not allowed to overwrite
    //           Overwrite, we are allowed to overwrite
    //           currently only used by CHK/DBG builds in an assert.
    //
    // Return Value:
    //    `true` if the key exists and was overwritten,
    //    `false` otherwise.
    //
    // Notes:
    //    If the key already exists and kind is Normal
    //    this method will assert
    //
    bool Set(Key k, Value v, SetKind kind = None)
    {
        CheckGrowth();

        assert(m_tableSizeInfo.prime != 0);

        unsigned index = GetIndexForKey(k);

        Node* pN = m_table[index];
        while ((pN != nullptr) && !KeyFuncs::Equals(k, pN->m_key))
        {
            pN = pN->m_next;
        }
        if (pN != nullptr)
        {
            assert(kind == Overwrite);
            pN->m_val = v;
            return true;
        }
        else
        {
            Node* pNewNode = new (m_alloc) Node(m_table[index], k, v);
            m_table[index] = pNewNode;
            m_tableCount++;
            return false;
        }
    }

    //------------------------------------------------------------------------
    // Emplace: Associates the specified key with a value constructed in-place
    // using the supplied args if the key is not already present.
    //
    // Arguments:
    //    k - the key
    //    args - the args used to construct the value
    //
    // Return Value:
    //    A pointer to the existing or newly constructed value.
    //
    template <class... Args>
    Value* Emplace(Key k, Args&&... args)
    {
        CheckGrowth();

        assert(m_tableSizeInfo.prime != 0);

        unsigned index = GetIndexForKey(k);

        Node* n = m_table[index];
        while ((n != nullptr) && !KeyFuncs::Equals(k, n->m_key))
        {
            n = n->m_next;
        }

        if (n == nullptr)
        {
            n = new (m_alloc) Node(m_table[index], k, std::forward<Args>(args)...);

            m_table[index] = n;
            m_tableCount++;
        }

        return &n->m_val;
    }

    //------------------------------------------------------------------------
    // Remove: Remove the specified key and its associated value.
    //
    // Arguments:
    //    k - the key
    //
    // Return Value:
    //    `true` if the key exists, `false` otherwise.
    //
    // Notes:
    //    Removing a inexistent key is not an error.
    //
    bool Remove(Key k)
    {
        unsigned index = GetIndexForKey(k);

        Node*  pN  = m_table[index];
        Node** ppN = &m_table[index];
        while ((pN != nullptr) && !KeyFuncs::Equals(k, pN->m_key))
        {
            ppN = &pN->m_next;
            pN  = pN->m_next;
        }
        if (pN != nullptr)
        {
            *ppN = pN->m_next;
            m_tableCount--;
            Node::operator delete(pN, m_alloc);
            return true;
        }
        else
        {
            return false;
        }
    }

    //------------------------------------------------------------------------
    // RemoveAll: Remove all keys and their associated values.
    //
    // Notes:
    //    This also frees all the memory owned by the table.
    //
    void RemoveAll()
    {
        for (unsigned i = 0; i < m_tableSizeInfo.prime; i++)
        {
            for (Node* pN = m_table[i]; pN != nullptr;)
            {
                Node* pNext = pN->m_next;
                Node::operator delete(pN, m_alloc);
                pN = pNext;
            }
        }
        m_alloc.deallocate(m_table);

        m_table         = nullptr;
        m_tableSizeInfo = JitPrimeInfo();
        m_tableCount    = 0;
        m_tableMax      = 0;
    }

    //
    // Iteration support
    //

    class NodeIterator
    {
    protected:
        Node**   m_table;
        Node*    m_node;
        unsigned m_tableSize;
        unsigned m_index;

        //------------------------------------------------------------------------
        // NodeIterator: Construct an iterator for the specified JitHashTable.
        //
        // Arguments:
        //    hash  - the hashtable
        //    begin - `true` to construct an "begin" iterator,
        //            `false` to construct an "end" iterator
        //
        NodeIterator(const JitHashTable* hash, bool begin)
            : m_table(hash->m_table)
            , m_node(nullptr)
            , m_tableSize(hash->m_tableSizeInfo.prime)
            , m_index(begin ? 0 : m_tableSize)
        {
            if (begin && (hash->m_tableCount > 0))
            {
                assert(m_table != nullptr);
                while ((m_index < m_tableSize) && (m_table[m_index] == nullptr))
                {
                    m_index++;
                }

                if (m_index < m_tableSize)
                {
                    m_node = m_table[m_index];
                    assert(m_node != nullptr);
                }
            }
        }

        //------------------------------------------------------------------------
        // Next: Advance the iterator to the next node.
        //
        // Notes:
        //    Advancing the end iterator has no effect.
        //
        void Next()
        {
            if (m_node != nullptr)
            {
                m_node = m_node->m_next;
                if (m_node != nullptr)
                {
                    return;
                }

                // Otherwise...
                m_index++;
            }
            while ((m_index < m_tableSize) && (m_table[m_index] == nullptr))
            {
                m_index++;
            }

            if (m_index < m_tableSize)
            {
                m_node = m_table[m_index];
                assert(m_node != nullptr);
            }
            else
            {
                m_node = nullptr;
            }
        }

    public:
        // Advance the iterator to the next node
        NodeIterator& operator++()
        {
            Next();
            return *this;
        }

        bool operator!=(const NodeIterator& i) const
        {
            return i.m_node != m_node;
        }
    };

    // KeyIterator: an iterator which yields only the hash table keys.
    class KeyIterator : public NodeIterator
    {
    public:
        KeyIterator(const JitHashTable* hash, bool begin)
            : NodeIterator(hash, begin)
        {
        }

        Key operator*() const
        {
            return this->m_node->GetKey();
        }
    };

    // ValueIterator: an iterator which yields only the hash table values.
    class ValueIterator : public NodeIterator
    {
    public:
        ValueIterator(const JitHashTable* hash, bool begin)
            : NodeIterator(hash, begin)
        {
        }

        Value operator*() const
        {
            return this->m_node->GetValue();
        }
    };

    // KeyValueIterator: an iterator which yields the hash table <key,value> pairs. It exposes a bit of the
    // hash table implementation by returning a `Node*` that contains the <key,value> data.
    class KeyValueIterator : public NodeIterator
    {
    public:
        KeyValueIterator(const JitHashTable* hash, bool begin)
            : NodeIterator(hash, begin)
        {
        }

        // We could return a new <key,value> struct, but why bother copying data?
        Node* operator*() const
        {
            return this->m_node;
        }
    };

    // KeyIteration: an adaptor to use for range-based `for` iteration over the hash table keys.
    class KeyIteration
    {
        const JitHashTable* const m_hash;

    public:
        KeyIteration(const JitHashTable* hash)
            : m_hash(hash)
        {
        }

        KeyIterator begin() const
        {
            return KeyIterator(m_hash, true);
        }

        KeyIterator end() const
        {
            return KeyIterator(m_hash, false);
        }
    };

    // ValueIteration: an adaptor to use for range-based `for` iteration over the hash table values.
    class ValueIteration
    {
        const JitHashTable* const m_hash;

    public:
        ValueIteration(const JitHashTable* hash)
            : m_hash(hash)
        {
        }

        ValueIterator begin() const
        {
            return ValueIterator(m_hash, true);
        }

        ValueIterator end() const
        {
            return ValueIterator(m_hash, false);
        }
    };

    // KeyValueIteration: an adaptor to use for range-based `for` iteration over the hash table <key,value> pairs.
    class KeyValueIteration
    {
        const JitHashTable* const m_hash;

    public:
        KeyValueIteration(const JitHashTable* hash)
            : m_hash(hash)
        {
        }

        KeyValueIterator begin() const
        {
            return KeyValueIterator(m_hash, true);
        }

        KeyValueIterator end() const
        {
            return KeyValueIterator(m_hash, false);
        }
    };

    // Get the number of keys currently stored in the table.
    unsigned GetCount() const
    {
        return m_tableCount;
    }

    // Get the allocator used by this hash table.
    Allocator GetAllocator()
    {
        return m_alloc;
    }

private:
    //------------------------------------------------------------------------
    // GetIndexForKey: Get the bucket index for the specified key.
    //
    // Arguments:
    //    k - the key
    //
    // Return Value:
    //    A bucket index
    //
    unsigned GetIndexForKey(Key k) const
    {
        unsigned hash = KeyFuncs::GetHashCode(k);

        unsigned index = m_tableSizeInfo.magicNumberRem(hash);

        return index;
    }

    //------------------------------------------------------------------------
    // FindNode: Return a pointer to the node having the specified key, if any.
    //
    // Arguments:
    //    k - the key
    //
    // Return Value:
    //    A pointer to the node or `nullptr` if the key is not found.
    //
    Node* FindNode(Key k) const
    {
        if (m_tableSizeInfo.prime == 0)
        {
            return nullptr;
        }

        unsigned index = GetIndexForKey(k);

        Node* pN = m_table[index];
        if (pN == nullptr)
        {
            return nullptr;
        }

        // Otherwise...
        while ((pN != nullptr) && !KeyFuncs::Equals(k, pN->m_key))
        {
            pN = pN->m_next;
        }

        assert((pN == nullptr) || KeyFuncs::Equals(k, pN->m_key));

        // If pN != nullptr, it's the node for the key, else the key isn't mapped.
        return pN;
    }

    //------------------------------------------------------------------------
    // Grow: Increase the size of the bucket table.
    //
    // Notes:
    //    The new size is computed based on the current population, growth factor,
    //    and maximum density factor.
    //
    void Grow()
    {
        unsigned newSize =
            (unsigned)(m_tableCount * Behavior::s_growth_factor_numerator / Behavior::s_growth_factor_denominator *
                       Behavior::s_density_factor_denominator / Behavior::s_density_factor_numerator);

        if (newSize < Behavior::s_minimum_allocation)
        {
            newSize = Behavior::s_minimum_allocation;
        }

        // handle potential overflow
        if (newSize < m_tableCount)
        {
            Behavior::NoMemory();
        }

        Reallocate(newSize);
    }

    //------------------------------------------------------------------------
    // CheckGrowth: Check if the maximum hashtable density has been reached
    // and increase the size of the bucket table if necessary.
    //
    void CheckGrowth()
    {
        if (m_tableCount == m_tableMax)
        {
            Grow();
        }
    }

public:
    //------------------------------------------------------------------------
    // Reallocate: Replace the bucket table with a larger one and copy all nodes
    // from the existing bucket table.
    //
    // Notes:
    //    The new size must be large enough to hold all existing keys in
    //    the table without exceeding the density. Note that the actual
    //    table size must always be a prime number; the specified size
    //    will be increased to the next prime if necessary.
    //
    void Reallocate(unsigned newTableSize)
    {
        assert(newTableSize >=
               (GetCount() * Behavior::s_density_factor_denominator / Behavior::s_density_factor_numerator));

        // Allocation size must be a prime number.  This is necessary so that hashes uniformly
        // distribute to all indices, and so that chaining will visit all indices in the hash table.
        JitPrimeInfo newPrime = NextPrime(newTableSize);
        newTableSize          = newPrime.prime;

        Node** newTable = m_alloc.template allocate<Node*>(newTableSize);

        for (unsigned i = 0; i < newTableSize; i++)
        {
            newTable[i] = nullptr;
        }

        // Move all entries over to new table (re-using the Node structures.)

        for (unsigned i = 0; i < m_tableSizeInfo.prime; i++)
        {
            Node* pN = m_table[i];
            while (pN != nullptr)
            {
                Node* pNext = pN->m_next;

                unsigned newIndex  = newPrime.magicNumberRem(KeyFuncs::GetHashCode(pN->m_key));
                pN->m_next         = newTable[newIndex];
                newTable[newIndex] = pN;

                pN = pNext;
            }
        }

        if (m_table != nullptr)
        {
            m_alloc.deallocate(m_table);
        }

        m_table         = newTable;
        m_tableSizeInfo = newPrime;
        m_tableMax =
            (unsigned)(newTableSize * Behavior::s_density_factor_numerator / Behavior::s_density_factor_denominator);
    }

    //------------------------------------------------------------------------
    // operator[]: Get a reference to the value associated with the specified key.
    //
    // Arguments:
    //    k - the key
    //
    // Return Value:
    //    A reference to the value associated with the specified key.
    //
    // Notes:
    //    The specified key must exist.
    //
    Value& operator[](Key k) const
    {
        Value* p = LookupPointer(k);
        assert(p);
        return *p;
    }

private:
    //------------------------------------------------------------------------
    // NextPrime: Get a prime number greater than or equal to the specified number.
    //
    // Arguments:
    //    number - the minimum value
    //
    // Return Value:
    //    A prime number.
    //
    static JitPrimeInfo NextPrime(unsigned number)
    {
        for (int i = 0; i < (int)(ArrLen(jitPrimeInfo)); i++)
        {
            if (jitPrimeInfo[i].prime >= number)
            {
                return jitPrimeInfo[i];
            }
        }

        // overflow
        Behavior::NoMemory();
    }

    // Instance members
    Allocator    m_alloc;         // Allocator to use in this table.
    Node**       m_table;         // pointer to table
    JitPrimeInfo m_tableSizeInfo; // size of table (a prime) and information about it
    unsigned     m_tableCount;    // number of elements in table
    unsigned     m_tableMax;      // maximum occupied count
};

// Commonly used KeyFuncs types:

// Base class for types whose equality function is the same as their "==".
template <typename T>
struct JitKeyFuncsDefEquals
{
    static bool Equals(const T& x, const T& y)
    {
        return x == y;
    }
};

template <typename T>
struct JitPtrKeyFuncs : public JitKeyFuncsDefEquals<const T*>
{
public:
    static unsigned GetHashCode(const T* ptr)
    {
        // Using the lower 32 bits of a pointer as a hashcode should be good enough.
        // In fact, this should result in an unique hash code unless we allocate
        // more than 4 gigabytes or if the virtual address space is fragmented.
        return static_cast<unsigned>(reinterpret_cast<uintptr_t>(ptr));
    }
};

template <typename T> // Must be coercible to "unsigned" with no loss of information.
struct JitSmallPrimitiveKeyFuncs : public JitKeyFuncsDefEquals<T>
{
    static unsigned GetHashCode(const T& val)
    {
        return static_cast<unsigned>(val);
    }
};

template <typename T> // Assumed to be of size sizeof(UINT64).
struct JitLargePrimitiveKeyFuncs : public JitKeyFuncsDefEquals<T>
{
    static unsigned GetHashCode(const T val)
    {
        // A static cast when T is a float or a double converts the value (i.e. 0.25 converts to 0)
        //
        // Instead we want to use all of the bits of a float to create the hash value
        // So we cast the address of val to a pointer to an equivalent sized unsigned int
        // This allows us to read the actual bit representation of a float type
        //
        // We can't read beyond the end of val, so we use sizeof(T) to determine
        // exactly how many bytes to read
        //
        if (sizeof(T) == 8)
        {
            // cast &val to (UINT64 *) then deref to get the bits
            UINT64 asUINT64 = *(reinterpret_cast<const UINT64*>(&val));

            // Get the upper and lower 32-bit values from the 64-bit value
            UINT32 upper32 = static_cast<UINT32>(asUINT64 >> 32);
            UINT32 lower32 = static_cast<UINT32>(asUINT64 & 0xFFFFFFFF);

            // Exclusive-Or the upper32 and the lower32 values
            return static_cast<unsigned>(upper32 ^ lower32);
        }
        else if (sizeof(T) == 4)
        {
            // cast &val to (UINT32 *) then deref to get the bits
            UINT32 asUINT32 = *(reinterpret_cast<const UINT32*>(&val));

            // Just return the 32-bit value
            return static_cast<unsigned>(asUINT32);
        }
        else if ((sizeof(T) == 2) || (sizeof(T) == 1))
        {
            // For small sizes we must have an integer type
            // so we can just use the static_cast.
            //
            return static_cast<unsigned>(val);
        }
        else
        {
            // Only support Hashing for types that are 8,4,2 or 1 bytes in size
            assert(!"Unsupported size");
            return static_cast<unsigned>(val); // compile-time error here when we have a illegal size
        }
    }
};
