/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
// Ownership order is deliberate: views are destroyed before their pinned cache
// handles, and the handles before their allocator. No per-token state is shared.
struct ObFTDictHub::BuiltinIKDicts
{
  BuiltinIKDicts()
      : allocator_(lib::ObMemAttr("FtsBuiltinDict")),
        descs_{{"main_dict", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN},
               {"quan_dict", ObFTDictType::DICT_IK_QUAN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN},
               {"stopword", ObFTDictType::DICT_IK_STOP, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN}},
        caches_{{allocator_}, {allocator_}, {allocator_}},
        dicts_{{allocator_, &caches_[0], descs_[0]},
               {allocator_, &caches_[1], descs_[1]},
               {allocator_, &caches_[2], descs_[2]}}
  {}

  int init(ObFTDictHub &hub)
  {
    int ret = OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      if (OB_FAIL(hub.load_cache(descs_[i], caches_[i]))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = hub.build_cache(descs_[i], caches_[i]);
        }
      }
      if (OB_SUCC(ret)) {
        ret = dicts_[i].init();
      }
    }
    return ret;
  }

  ObArenaAllocator allocator_;
  ObFTDictDesc descs_[3];
  ObFTCacheRangeContainer caches_[3];
  ObFTRangeDict dicts_[3];
};

ObFTDictHub::~ObFTDictHub()
{
  destroy();
}

int ObFTDictHub::get_builtin_ik_dicts(const ObIFTDict *&main_dict,
                                     const ObIFTDict *&quan_dict,
                                     const ObIFTDict *&stop_dict)
{
  int ret = OB_SUCCESS;
  BuiltinIKDicts *dicts = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (nullptr == (dicts = builtin_ik_dicts_.load(std::memory_order_acquire))) {
    lib::ObMutexGuard guard(builtin_ik_lock_);
    if (nullptr == (dicts = builtin_ik_dicts_.load(std::memory_order_relaxed))) {
      if (OB_ISNULL(dicts = OB_NEW(BuiltinIKDicts, "FtsBuiltinDict"))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(dicts->init(*this))) {
        // Never publish a partial view. A later call can retry initialization.
        OB_DELETE(BuiltinIKDicts, "FtsBuiltinDict", dicts);
        dicts = nullptr;
      } else {
        builtin_ik_dicts_.store(dicts, std::memory_order_release);
      }
    }
  }
  if (OB_SUCC(ret)) {
    main_dict = &dicts->dicts_[0];
    quan_dict = &dicts->dicts_[1];
    stop_dict = &dicts->dicts_[2];
  }
  return ret;
}

int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  // The server calls this only after parser users have stopped.
  BuiltinIKDicts *dicts = builtin_ik_dicts_.exchange(nullptr, std::memory_order_relaxed);
  if (nullptr != dicts) {
    OB_DELETE(BuiltinIKDicts, "FtsBuiltinDict", dicts);
  }
  is_inited_ = false;
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
