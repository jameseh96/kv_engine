/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#pragma once
#define MEMCACHED_ENGINE_H

#include <sys/types.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <gsl/gsl>

#include "memcached/allocator_hooks.h"
#include "memcached/callback.h"
#include "memcached/collections.h"
#include "memcached/config_parser.h"
#include "memcached/dockey.h"
#include "memcached/engine_common.h"
#include "memcached/extension.h"
#include "memcached/protocol_binary.h"
#include "memcached/server_api.h"
#include "memcached/types.h"
#include "memcached/vbucket.h"

/*! \mainpage memcached public API
 *
 * \section intro_sec Introduction
 *
 * The memcached project provides an API for providing engines as well
 * as data definitions for those implementing the protocol in C.  This
 * documentation will explain both to you.
 *
 * \section docs_sec API Documentation
 *
 * Jump right into <a href="modules.html">the modules docs</a> to get started.
 *
 * \example default_engine.cc
 */

/**
 * \defgroup Engine Storage Engine API
 * \defgroup Protex Protocol Extension API
 * \defgroup Protocol Binary Protocol Structures
 *
 * \addtogroup Engine
 * @{
 *
 * Most interesting here is to implement engine_interface_v1 for your
 * engine.
 */

/**
 * Abstract interface to an engine.
 */

/* This is typedefed in types.h */
struct server_handle_v1_t {
    ServerCoreIface* core;
    ServerCallbackIface* callback;
    ServerLogIface* log;
    SERVER_COOKIE_API* cookie;
    ALLOCATOR_HOOKS_API* alloc_hooks;
    ServerDocumentIface* document;
};

/**
 * The signature for the "create_instance" function exported from the module.
 *
 * This function should fill out an engine interface structure.
 *
 * @param get_server_api function to get the server API from
 * @param Where to store the interface handle
 * @return See description of ENGINE_ERROR_CODE
 */
typedef ENGINE_ERROR_CODE (*CREATE_INSTANCE)(GET_SERVER_API get_server_api,
                                             EngineIface** handle);

/**
 * The signature for the "destroy_engine" function exported from the module.
 *
 * This function is called prior to closing of the module. This function should
 * free any globally allocated resources.
 *
 */
typedef void (* DESTROY_ENGINE)(void);

/**
 * A unique_ptr to use with items returned from the engine interface.
 */
namespace cb {
class ItemDeleter;
typedef std::unique_ptr<item, ItemDeleter> unique_item_ptr;

using EngineErrorItemPair = std::pair<cb::engine_errc, cb::unique_item_ptr>;

using EngineErrorMetadataPair = std::pair<engine_errc, item_info>;

enum class StoreIfStatus {
    Continue,
    Fail,
    GetItemInfo // please get me the item_info
};

using StoreIfPredicate = std::function<StoreIfStatus(
        const boost::optional<item_info>&, cb::vbucket_info)>;

struct EngineErrorCasPair {
    engine_errc status;
    uint64_t cas;
};
}

/**
 * The different compression modes that a bucket supports
 */
enum class BucketCompressionMode : uint8_t {
    Off,     //Data will be stored as uncompressed
    Passive, //Data will be stored as provided by the client
    Active   //Bucket will actively try to compress stored
             //data
};

/* The default minimum compression ratio */
static const float default_min_compression_ratio = 1.2f;

/* The default maximum size for a value */
static const size_t default_max_item_size = 20 * 1024 * 1024;

/**
 * Definition of the first version of the engine interface
 */
struct MEMCACHED_PUBLIC_CLASS EngineIface {
    virtual ~EngineIface() = default;

    /**
     * Initialize an engine instance.
     * This is called *after* creation, but before the engine may be used.
     *
     * @param handle the engine handle
     * @param config_str configuration this engine needs to initialize itself.
     */
    virtual ENGINE_ERROR_CODE initialize(const char* config_str) = 0;

    /**
     * Tear down this engine.
     *
     * @param force the flag indicating the force shutdown or not.
     */
    virtual void destroy(bool force) = 0;

    /*
     * Item operations.
     */

    /**
     * Allocate an item.
     *
     * @param cookie The cookie provided by the frontend
     * @param output variable that will receive the item
     * @param key the item's key
     * @param nbytes the number of bytes that will make up the
     *        value of this item.
     * @param flags the item's flags
     * @param exptime the maximum lifetime of this item
     * @param vbucket virtual bucket to request allocation from
     *
     * @return {cb::engine_errc::success, unique_item_ptr} if all goes well
     */
    virtual cb::EngineErrorItemPair allocate(gsl::not_null<const void*> cookie,
                                             const DocKey& key,
                                             const size_t nbytes,
                                             const int flags,
                                             const rel_time_t exptime,
                                             uint8_t datatype,
                                             uint16_t vbucket) = 0;

    /**
     * Allocate an item (extended API)
     *
     * @param cookie The cookie provided by the frontend
     * @param key the item's key
     * @param nbytes the number of bytes that will make up the
     *               value of this item.
     * @param priv_nbytes The number of bytes in nbytes containing
     *                    system data (and may exceed the item limit).
     * @param flags the item's flags
     * @param exptime the maximum lifetime of this item
     * @param vbucket virtual bucket to request allocation from
     * @return pair containing the item and the items information
     * @thows cb::engine_error with:
     *
     *   * `cb::engine_errc::no_bucket` The client is bound to the dummy
     *                                  `no bucket` which don't allow
     *                                  allocations.
     *
     *   * `cb::engine_errc::no_memory` The bucket is full
     *
     *   * `cb::engine_errc::too_big` The requested memory exceeds the
     *                                limit set for items in the bucket.
     *
     *   * `cb::engine_errc::disconnect` The client should be disconnected
     *
     *   * `cb::engine_errc::not_my_vbucket` The requested vbucket belongs
     *                                       to someone else
     *
     *   * `cb::engine_errc::temporary_failure` Temporary failure, the
     *                                          _client_ should try again
     *
     *   * `cb::engine_errc::too_busy` Too busy to serve the request,
     *                                 back off and try again.
     */
    virtual std::pair<cb::unique_item_ptr, item_info> allocate_ex(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            size_t nbytes,
            size_t priv_nbytes,
            int flags,
            rel_time_t exptime,
            uint8_t datatype,
            uint16_t vbucket) = 0;

    /**
     * Remove an item.
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key identifying the item to be removed
     * @param vbucket the virtual bucket id
     * @param mut_info On a successful remove write the mutation details to
     *                 this address.
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE remove(gsl::not_null<const void*> cookie,
                                     const DocKey& key,
                                     uint64_t& cas,
                                     uint16_t vbucket,
                                     mutation_descr_t& mut_info) = 0;

    /**
     * Indicate that a caller who received an item no longer needs
     * it.
     *
     * @param item the item to be released
     */
    virtual void release(gsl::not_null<item*> item) = 0;

    /**
     * Retrieve an item.
     *
     * @param cookie The cookie provided by the frontend
     * @param item output variable that will receive the located item
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     * @param documentStateFilter The document to return must be in any of
     *                            of these states. (If `Alive` is set, return
     *                            KEY_ENOENT if the document in the engine
     *                            is in another state)
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual cb::EngineErrorItemPair get(gsl::not_null<const void*> cookie,
                                        const DocKey& key,
                                        uint16_t vbucket,
                                        DocStateFilter documentStateFilter) = 0;

    /**
     * Optionally retrieve an item. Only non-deleted items may be fetched
     * through this interface (Documents in deleted state may be evicted
     * from memory and we don't want to go to disk in order to fetch these)
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     * @param filter callback filter to see if the item should be returned
     *               or not. If filter returns false the item should be
     *               skipped.
     *               Note: the filter is applied only to the *metadata* of the
     *               item_info - i.e. the `value` should not be expected to be
     *               present when filter is invoked.
     * @return A pair of the error code and (optionally) the item
     */
    virtual cb::EngineErrorItemPair get_if(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket,
            std::function<bool(const item_info&)> filter) = 0;

    /**
     * Retrieve metadata for a given item.
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     *
     * @return  Pair (ENGINE_SUCCESS, Metadata) if all goes well
     */
    virtual cb::EngineErrorMetadataPair get_meta(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket) = 0;

    /**
     * Lock and Retrieve an item.
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     * @param lock_timeout the number of seconds to hold the lock
     *                     (0 == use the engines default lock time)
     *
     * @return A pair of the error code and (optionally) the item
     */
    virtual cb::EngineErrorItemPair get_locked(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket,
            uint32_t lock_timeout) = 0;

    /**
     * Unlock an item.
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     * @param cas the cas value for the locked item
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE unlock(gsl::not_null<const void*> cookie,
                                     const DocKey& key,
                                     uint16_t vbucket,
                                     uint64_t cas) = 0;

    /**
     * Get and update the expiry time for the document
     *
     * @param cookie The cookie provided by the frontend
     * @param key the key to look up
     * @param vbucket the virtual bucket id
     * @param expirytime the new expiry time for the object
     * @return A pair of the error code and (optionally) the item
     */
    virtual cb::EngineErrorItemPair get_and_touch(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket,
            uint32_t expirytime) = 0;

    /**
     * Store an item into the underlying engine with the given
     * state. If the DocumentState is set to DocumentState::Deleted
     * the document shall not be returned unless explicitly asked for
     * documents in that state, and the underlying engine may choose to
     * purge it whenever it please.
     *
     * @param cookie The cookie provided by the frontend
     * @param item the item to store
     * @param cas the CAS value for conditional sets
     * @param operation the type of store operation to perform.
     * @param document_state The state the document should have after
     *                       the update
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE store(gsl::not_null<const void*> cookie,
                                    gsl::not_null<item*> item,
                                    uint64_t& cas,
                                    ENGINE_STORE_OPERATION operation,
                                    DocumentState document_state) = 0;

    /**
     * Store an item into the underlying engine with the given
     * state only if the predicate argument returns true when called against an
     * existing item.
     *
     * Optional interface; not supported by all engines.
     *
     * @param cookie The cookie provided by the frontend
     * @param item the item to store
     * @param cas the CAS value for conditional sets
     * @param operation the type of store operation to perform.
     * @param predicate a function that will be called from the engine the
     *                  result of which determines how the store behaves.
     *                  The function is given any existing item's item_info (as
     *                  a boost::optional) and a cb::vbucket_info object. In the
     *                  case that the optional item_info is not initialised the
     *                  function can return cb::StoreIfStatus::GetInfo to
     *                  request that the engine tries to get the item_info, the
     *                  engine may ignore this return code if it knows better
     *                  i.e. a memory only engine and no item_info can be
     *                  fetched. The function can also return ::Fail if it
     *                  wishes to fail the store_if (returning predicate_failed)
     *                  or the predicate can return ::Continue and the rest of
     *                  the store_if will execute (and possibly fail for other
     *                  reasons).
     * @param document_state The state the document should have after
     *                       the update
     *
     * @return a std::pair containing the engine_error code and new CAS
     */
    virtual cb::EngineErrorCasPair store_if(gsl::not_null<const void*> cookie,
                                            gsl::not_null<item*> item,
                                            uint64_t cas,
                                            ENGINE_STORE_OPERATION operation,
                                            cb::StoreIfPredicate predicate,
                                            DocumentState document_state) {
        return {cb::engine_errc::not_supported, 0};
    }

    /**
     * Flush the cache.
     *
     * Optional interface; not supported by all engines.
     *
     * @param cookie The cookie provided by the frontend
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE flush(gsl::not_null<const void*> cookie) {
        return ENGINE_ENOTSUP;
    }

    /*
     * Statistics
     */

    /**
     * Get statistics from the engine.
     *
     * @param cookie The cookie provided by the frontend
     * @param key optional argument to stats
     * @param add_stat callback to feed results to the output
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE get_stats(gsl::not_null<const void*> cookie,
                                        cb::const_char_buffer key,
                                        ADD_STAT add_stat) = 0;

    /**
     * Reset the stats.
     *
     * @param cookie The cookie provided by the frontend
     */
    virtual void reset_stats(gsl::not_null<const void*> cookie) = 0;

    /**
     * Any unknown command will be considered engine specific.
     *
     * @param cookie The cookie provided by the frontend
     * @param request pointer to request header to be filled in
     * @param response function to transmit data
     *
     * @return ENGINE_SUCCESS if all goes well
     */
    virtual ENGINE_ERROR_CODE unknown_command(
            const void* cookie,
            gsl::not_null<protocol_binary_request_header*> request,
            ADD_RESPONSE response) {
        return ENGINE_ENOTSUP;
    }

    /**
     * Set the CAS id on an item.
     */
    virtual void item_set_cas(gsl::not_null<item*> item, uint64_t cas) = 0;

    /**
     * Set the data type on an item.
     */
    virtual void item_set_datatype(gsl::not_null<item*> item,
                                   protocol_binary_datatype_t datatype) = 0;

    /**
     * Get information about an item.
     *
     * The loader of the module may need the pointers to the actual data within
     * an item. Instead of having to create multiple functions to get each
     * individual item, this function will get all of them.
     *
     * @param item the item to request information about
     * @param item_info
     * @return true if successful
     */
    virtual bool get_item_info(gsl::not_null<const item*> item,
                               gsl::not_null<item_info*> item_info) = 0;

    /**
     * Set the current log level
     *
     * @param level the current log level
     */
    virtual void set_log_level(EXTENSION_LOG_LEVEL level){};

    collections_interface collections;

    /**
     * @returns if XATTRs are enabled for this bucket
     */
    virtual bool isXattrEnabled() {
        return false;
    }

    /**
     * @returns the compression mode of the bucket
     */
    virtual BucketCompressionMode getCompressionMode() {
        return BucketCompressionMode::Off;
    }

    /**
     * @returns the maximum item size supported by the bucket
     */
    virtual size_t getMaxItemSize() {
        return default_max_item_size;
    };

    /**
     * @returns the minimum compression ratio defined in the bucket
     */
    virtual float getMinCompressionRatio() {
        return default_min_compression_ratio;
    }
};

namespace cb {
class ItemDeleter {
public:
    ItemDeleter() : handle(nullptr) {}

    /**
     * Create a new instance of the item deleter.
     *
     * @param handle_ the handle to the the engine who owns the item
     */
    ItemDeleter(EngineIface* handle_) : handle(handle_) {
        if (handle == nullptr) {
            throw std::invalid_argument(
                "cb::ItemDeleter: engine handle cannot be nil");
        }
    }

    /**
     * Create a copy constructor to allow us to use std::move of the item
     */
    ItemDeleter(const ItemDeleter& other) : handle(other.handle) {
    }

    void operator()(item* item) {
        if (handle) {
            handle->release(item);
        } else {
            throw std::invalid_argument("cb::ItemDeleter: item attempted to be "
                                        "freed by null engine handle");
        }
    }

private:
    EngineIface* handle;
};

inline EngineErrorItemPair makeEngineErrorItemPair(cb::engine_errc err) {
    return {err, unique_item_ptr{nullptr, ItemDeleter{}}};
}

inline EngineErrorItemPair makeEngineErrorItemPair(cb::engine_errc err,
                                                   item* it,
                                                   EngineIface* handle) {
    return {err, unique_item_ptr{it, ItemDeleter{handle}}};
}
}

/**
 * @}
 */
