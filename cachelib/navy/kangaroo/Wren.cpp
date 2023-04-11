#include <mutex>
#include <shared_mutex>

#include "cachelib/navy/kangaroo/Wren.h"

namespace facebook {
namespace cachelib {
namespace navy {

Wren::EuIterator Wren::getEuIterator() {
  for (uint64_t i = 0; i < numBuckets_; i++) {
    if (kbidToEuid_[i].euid_.index() / bucketsPerEu_ == eraseEraseUnit_) {
      return EuIterator(KangarooBucketId(i));
    }
  }
  return EuIterator();
}

Wren::EuIterator Wren::getNext(EuIterator euit) {
  for (uint64_t i = euit.getBucket().index() + 1; i < numBuckets_; i++) {
    if (kbidToEuid_[i].euid_.index() / bucketsPerEu_ == eraseEraseUnit_) {
      //XLOGF(INFO, "Kbid is {}, euid {}, zone {}", i, kbidToEuid_[i].euid_.index(), 
      //    kbidToEuid_[i].euid_.index() / bucketsPerEu_);
      //XLOGF(INFO, "Time to move kbid {}", i);
      return EuIterator(KangarooBucketId(i));
    }
  }
  //XLOG(INFO, "Return false");
  return EuIterator();
}

Wren::Wren(Device& device, uint64_t numBuckets, uint64_t bucketSize, uint64_t totalSize, uint64_t setOffset)
          : device_{device}, 
          euCap_{device_.getIOZoneCapSize()},
          numEus_{totalSize / euCap_},
          eraseEraseUnit_{numEus_ - 1},
          numBuckets_{numBuckets}, 
          bucketSize_{bucketSize},
          setOffset_{setOffset},
          bucketsPerEu_{euCap_ / bucketSize_} {
  kbidToEuid_ = new EuIdentifier[numBuckets_];
  XLOGF(INFO, "Num WREN zones {} from size {}, starting at writeEraseUnit_ {} til eraseEraseUnit_ {}", 
      numEus_, totalSize, writeEraseUnit_, eraseEraseUnit_);
}

Wren::~Wren() {
  delete kbidToEuid_;
}

Wren::EuId Wren::calcEuId(uint32_t erase_unit, uint32_t offset) {
  uint64_t euOffset = erase_unit * bucketsPerEu_;
  return EuId(euOffset + offset);
}

Wren::EuId Wren::findEuId(KangarooBucketId kbid) {
  XDCHECK(kbid.index() < numBuckets_);
  return kbidToEuid_[kbid.index()].euid_;
}

uint64_t Wren::getEuIdLoc(uint32_t erase_unit, uint32_t offset) {
  return getEuIdLoc(calcEuId(erase_unit, offset));
}

uint64_t Wren::getEuIdLoc(EuId euid) {
  uint64_t zone_offset = euid.index() % bucketsPerEu_;
  uint64_t zone = euid.index() / bucketsPerEu_;
  uint64_t offset = setOffset_ + zone_offset * bucketSize_ 
    + zone * device_.getIOZoneSize();
}

Buffer Wren::read(KangarooBucketId kbid) {
  EuId euid = findEuId(kbid);
  if (euid.index() >= numEus_ * euCap_) {
    // kbid has not yet been defined
    return device_.makeIOBuffer(bucketSize_);
  }
  uint64_t loc = getEuIdLoc(euid);

  auto buffer = device_.makeIOBuffer(bucketSize_);
  XDCHECK(!buffer.isNull());

  const bool res = device_.read(loc, buffer.size(), buffer.data());
  if (!res) {
    return {};
  }

  return buffer;
}

bool Wren::write(KangarooBucketId kbid, Buffer buffer) {
  {
    // TODO: deserialize
    std::unique_lock<folly::SharedMutex> lock{writeMutex_};
    if (writeEraseUnit_ == eraseEraseUnit_) {
        XLOG(INFO, "WREN Writing caught up to erasing");
        exit(1);
        return false;
    }
  
    if (writeOffset_ == 0) {
      XLOGF(INFO, "WREN Write: reseting zone {}, {} / {}", 
          getEuIdLoc(writeEraseUnit_, 0)/device_.getIOZoneSize(),
          writeEraseUnit_, numEus_);
      device_.reset(getEuIdLoc(writeEraseUnit_, 0), device_.getIOZoneSize());
    }

    // TODO: need to update chain before changing and deal with synchronization
    EuId euid = calcEuId(writeEraseUnit_, writeOffset_);
    uint64_t loc = getEuIdLoc(euid);
    //XLOGF(INFO, "WREN: Writing {} bucket to zone {}, offset {}, location {}", 
    //    kbid.index(), loc / device_.getIOZoneSize(), 
    //    (loc % device_.getIOZoneSize()) / bucketSize_, loc);
    XDCHECK(euid.index() < numEus_ * euCap_);
    kbidToEuid_[kbid.index()].euid_ = euid;

    bool ret = device_.write(loc, std::move(buffer));
    if (!ret) {
      XLOGF(INFO, "tried to write at {} euid, {}.{} calculated zone + offset, write zone {}, loc {}", 
          euid.index(), euid.index() / (euCap_/ bucketSize_), euid.index() % (euCap_/bucketSize_),
          writeEraseUnit_, loc);
    }
    
    writeOffset_++;
    if (writeOffset_ >= euCap_/bucketSize_) {
      device_.finish(getEuIdLoc(writeEraseUnit_, 0), device_.getIOZoneSize());
      //XLOGF(INFO, "WREN Write: finishing zone {} old eu {} / {}", 
      //    getEuIdLoc(writeEraseUnit_, 0)/device_.getIOZoneSize(), writeEraseUnit_, numEus_);
      writeEraseUnit_ = (writeEraseUnit_ + 1) % numEus_;
      XLOGF(INFO, "WREN Write: new zone {} new eu {} / {}", 
          getEuIdLoc(writeEraseUnit_, 0)/device_.getIOZoneSize(), writeEraseUnit_, numEus_);
      writeOffset_ = 0;
    }

    return ret;

  }
}

bool Wren::shouldClean(double cleaningThreshold) {
  uint32_t freeEus = 0;
  uint32_t writeEu = writeEraseUnit_;
  if (eraseEraseUnit_ >= writeEu) {
    freeEus = eraseEraseUnit_ - writeEu;
  } else {
    freeEus = eraseEraseUnit_ + (numEus_ - writeEu);
  }
  return freeEus <= cleaningThreshold * numEus_;
}

bool Wren::erase() {
  EuId euid = calcEuId(eraseEraseUnit_, 0);
  eraseEraseUnit_ = (eraseEraseUnit_ + 1) % numEus_;
  XLOGF(INFO, "WREN Erase: new zone {} new eu {} / {}", 
          getEuIdLoc(eraseEraseUnit_, 0)/device_.getIOZoneSize(), eraseEraseUnit_, numEus_);
  return device_.reset(euid.index(), euCap_);
}

} // namespace navy
} // namespace cachelib
} // namespace facebook
