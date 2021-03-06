#define BOOST_LOG_DYN_LINK 1

#include <iostream>
#include <random>
#include <unistd.h>
#include <fstream>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/mgmt/nfd/controller.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/ims/in-memory-storage-persistent.hpp>
#include <ndn-cxx/security/command-interest-signer.hpp>

#include <boost/filesystem.hpp>

#include <PSync/logging.hpp>

#include "../src/repo-command-parameter.hpp"
#include "../src/repo-command-response.hpp"

using namespace ndn;
using namespace repo;

_LOG_INIT(DataCollector);

class DeviceSigner
{
public:
  DeviceSigner(std::string& deviceName, Name& prefix, std::string& repoName)
  : m_scheduler(m_face.getIoService())
  , m_deviceName(deviceName)
  // /<BigCompany>/<Building1>/<ConfRoom>/sensor/<sensorName>/<sensorType>/<timestamp>
  , m_prefix(Name(prefix).append(repoName).append(m_deviceName)) // Key Name prefix
  , m_temperatureI(Name(m_prefix).append("temperature")) // Data Name
  , m_humidityI(Name(m_prefix).append("humidity"))
  , m_pressureI(Name(m_prefix).append("pressure"))
  , m_resistanceI(Name(m_prefix).append("resistance"))
  , m_occupancyI(Name(m_prefix).append("occupancy"))
  , m_repoPrefix(Name("localhost").append(repoName))
  , m_seqFileName("home/pi/repo-ng/seq/")
  , m_cmdSigner(m_keyChain)
  {
    m_seqFileName.append(m_deviceName);
    m_seqFileName.append(".seq");
    initiateSeqFromFile();

    insertFirstData();
  }

  void
  initiateSeqFromFile() {
    std::ifstream inputFile(m_seqFileName.c_str());
    if (inputFile.good()) {
      std::string sensorSeq;
      uint32_t seqNo = 0;

      inputFile >> sensorSeq >> seqNo;
      m_tempSeq = seqNo;

      inputFile >> sensorSeq >> seqNo;
      m_humidSeq= seqNo;

      inputFile >> sensorSeq >> seqNo;
      m_pressureSeq = seqNo;

      inputFile >> sensorSeq >> seqNo;
      m_resistanceSeq = seqNo;

      inputFile >> sensorSeq >> seqNo;
      m_occupancySeq = seqNo;
      
    } else {
      writeSeqToFile();
    }
    inputFile.close();
  }

  void writeSeqToFile() {
    std::ofstream outputFile(m_seqFileName.c_str());
    std::ostringstream os;
    os << "TempSeq " << std::to_string(m_tempSeq) << "\n"
       << "HumidSeq "  << std::to_string(m_humidSeq)  << "\n"
       << "PressureSeq "  << std::to_string(m_pressureSeq) << "\n"
       << "ResistanceSeq " << std::to_string(m_resistanceSeq) << "\n"
       << "OccupancySeq " << std::to_string(m_occupancySeq);
    outputFile << os.str();
    outputFile.close();
  }

  void
  insertFirstData()
  {
    // _LOG_DEBUG("Inserting first data " << m_temperatureI << " " << m_humidityI);
    std::cout << "Sending interests to insert first data into repo." << std::endl;
    std::cout << m_temperatureI.toUri() << std::endl;
    std::cout << m_humidityI.toUri() << std::endl;
    std::cout << m_pressureI.toUri() << std::endl;
    std::cout << m_resistanceI.toUri() << std::endl;
    std::cout << m_occupancyI.toUri() << std::endl;
    sendInterest(m_temperatureI);
    sendInterest(m_humidityI);
    sendInterest(m_pressureI);
    sendInterest(m_resistanceI);
    sendInterest(m_occupancyI);
  }

  void
  sendInterest(const Name& interestName)
  {
    // All this just for log purposes
    Name dataName;

    Name interestNameWithTimestampAndSeqNo(interestName);

    if (interestName == m_temperatureI) {
      interestNameWithTimestampAndSeqNo.appendTimestamp();
      interestNameWithTimestampAndSeqNo.appendNumber(m_tempSeq++);
   
    }
    else if (interestName == m_humidityI) {
      interestNameWithTimestampAndSeqNo.appendTimestamp();
      interestNameWithTimestampAndSeqNo.appendNumber(m_humidSeq++);
   
    }
    else if (interestName == m_pressureI) {
      interestNameWithTimestampAndSeqNo.appendTimestamp();
      interestNameWithTimestampAndSeqNo.appendNumber(m_pressureSeq++);
    }
    else if (interestName == m_resistanceI) {
      interestNameWithTimestampAndSeqNo.appendTimestamp();
      interestNameWithTimestampAndSeqNo.appendNumber(m_resistanceSeq++);
    }
    else if (interestName == m_occupancyI) {
      interestNameWithTimestampAndSeqNo.appendTimestamp();
      interestNameWithTimestampAndSeqNo.appendNumber(m_occupancySeq++);
    }

    writeSeqToFile();

    std::cout << "Attempting to insert data with following name into repo: " << std::endl;
    std::cout << interestNameWithTimestampAndSeqNo.toUri() << std::endl;
    
    insertIntoRepo(interestNameWithTimestampAndSeqNo);
    
    m_scheduler.scheduleEvent(ndn::time::milliseconds(10000), std::bind(&DeviceSigner::sendInterest, this, interestName));

  }

  void
  insertIntoRepo(Name dataName) {
    RepoCommandParameter parameters;
    parameters.setName(dataName);

    // Generate command interest
    Interest cmdInterest;
    Name cmd = m_repoPrefix;
    cmd
    .append("insert")
    .append(parameters.wireEncode());

    cmdInterest = m_cmdSigner.makeCommandInterest(cmd);

    cmdInterest.setInterestLifetime(time::milliseconds(4000));

    m_face.expressInterest(cmdInterest,
                           nullptr,
                           [] (const Interest& interest, const ndn::lp::Nack& nack) {
                             //_LOG_INFO("Nack from repo " << nack.getReason());
			     std::cout << "Nack from repo " << nack.getReason() << std::endl;
                           },
                           [] (const Interest& interest) {
                             //_LOG_INFO("Timeout from repo");
			     std::cout << "Timeout from repo" << std::endl;
                           });
  }

  void run()
  {
    try {
      m_face.processEvents();
    } catch (std::runtime_error& e) {
      std::cerr << e.what() << std::endl;
      return;
    }
  }

private:
  Face m_face;
  Scheduler m_scheduler;
  KeyChain m_keyChain;

  // Device name is used to create a face to the device
  // Prefix is the prefix of the data name that the device is listening to
  std::string m_deviceName;
  Name m_prefix;
  Name m_temperatureI, m_humidityI, m_pressureI, m_resistanceI, m_occupancyI;
  uint32_t m_tempSeq = 0, m_humidSeq = 0, m_pressureSeq = 0, m_resistanceSeq = 0, m_occupancySeq = 0;

  ndn::InMemoryStoragePersistent m_ims;
  ndn::Name m_repoPrefix;
  std::string m_seqFileName;
  ndn::security::CommandInterestSigner m_cmdSigner;
};

int main(int argc, char* argv[]) {
  if ( argc != 4 ) {
    std::cout << " Usage: " << argv[0]
              << " <deviceName> <prefix - /Company/building/roomNumber> <repoName>\n";
  }
  else {
    std::string deviceName(argv[1]);
    Name prefix(argv[2]);
    std::string repoName(argv[3]);
    DeviceSigner ds(deviceName, prefix, repoName);
    ds.run();
  }
}
