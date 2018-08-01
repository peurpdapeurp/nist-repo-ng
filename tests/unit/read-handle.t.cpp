/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2017,  Regents of the University of California.
 *
 * This file is part of NDN repo-ng (Next generation of NDN repository).
 * See AUTHORS.md for complete list of repo-ng authors and contributors.
 *
 * repo-ng is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * repo-ng is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * repo-ng, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "handles/read-handle.hpp"
#include "storage/sqlite-storage.hpp"
#include "storage/repo-storage.hpp"

#include "../repo-storage-fixture.hpp"

#include <boost/test/unit_test.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#define CHECK_INTERESTS(NAME,COMPONENT,BOOL)                  \
  do {                                                        \
    didMatch = false;                                         \
    for (const auto interest : face.sentInterests) {          \
      bool isPresent = false;                                 \
      for (const auto section : NAME) {                       \
        isPresent = isPresent || section == COMPONENT;        \
      }                                                       \
      didMatch = didMatch || isPresent;                       \
    }                                                         \
    BOOST_CHECK_EQUAL(didMatch,BOOL);                         \
  } while (0)

namespace repo {
namespace tests {

BOOST_AUTO_TEST_SUITE(TestReadHandle)

class Fixture : public RepoStorageFixture
{
public:
  Fixture()
    : face(ndn::util::DummyClientFace::Options{true, true})
    , scheduler(face.getIoService())
    , subsetLength(1)
    , dataPrefix("/ndn/test/prefix")
    , identity("/ndn/test/identity")
    , readHandle(face, *handle, keyChain, scheduler, subsetLength)
    , numPrefixRegistrations(0)
    , numPrefixUnregistrations(0)
  {
    readHandle.connectAutoListen();
  }

public:
  bool
  containsNameComponent(const Name& name, const ndn::name::Component& component)
  {
    bool isPresent = false;
    for (const auto section : name) {
      isPresent = isPresent || section == component;
    }
    return isPresent;
  }

public:
  ndn::util::DummyClientFace face;
  ndn::KeyChain keyChain;
  ndn::Scheduler scheduler;

  size_t subsetLength;
  ndn::Name dataPrefix;
  ndn::Name identity;
  ReadHandle readHandle;

  size_t numPrefixRegistrations;
  size_t numPrefixUnregistrations;
};

BOOST_FIXTURE_TEST_CASE(DataPrefixes, Fixture)
{
  const std::vector<uint8_t> content(100, 'x');
  std::shared_ptr<Data> data1 = std::make_shared<Data>(Name{dataPrefix}.appendNumber(1));
  std::shared_ptr<Data> data2 = std::make_shared<Data>(Name{dataPrefix}.appendNumber(2));

  data1->setContent(&content[0], content.size());
  data2->setContent(&content[0], content.size());

  keyChain.createIdentity(identity);
  keyChain.sign(*data1, ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                                  identity));
  keyChain.sign(*data2, ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                                  identity));

  bool didMatch = false;
  face.sentInterests.clear();
  handle->insertData(*data1);
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"register"}, true);

  face.sentInterests.clear();
  handle->deleteData(data1->getFullName());
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"unregister"}, true);

  face.sentInterests.clear();
  handle->insertData(*data1);
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"register"}, true);

  face.sentInterests.clear();
  handle->insertData(*data2);
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"register"}, false);

  face.sentInterests.clear();
  handle->deleteData(data1->getFullName());
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"unregister"}, false);

  face.sentInterests.clear();
  handle->deleteData(data2->getFullName());
  face.processEvents(ndn::time::milliseconds(-1));
  CHECK_INTERESTS(interest.getName(), name::Component{"unregister"}, true);
}

BOOST_AUTO_TEST_SUITE_END() // TestReadHandle

} // namespace tests
} // namespace repo
