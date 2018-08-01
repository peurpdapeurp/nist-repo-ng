#define BOOST_LOG_DYN_LINK 1

#include "../src/repo-command-parameter.hpp"
#include "../src/repo-command-response.hpp"
#include <iostream>

#include <PSync/logging.hpp>

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/notification-subscriber.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/security/command-interest-signer.hpp>

#include <ChronoSync/socket.hpp>

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>

std::mutex systemInfoWriteMutex;

using namespace ndn;
using namespace repo;

_LOG_INIT(ChronoApp);

class ChronoApp
{
public:
  ChronoApp(Name pubSubGroupPrefix, Name repoName)
  : m_pubSubGroupPrefix(pubSubGroupPrefix)
  , m_repoName(repoName)
  // /<group-prefix>/repo1/chronosync
  , m_chronoSyncUserPrefix(Name(pubSubGroupPrefix).append(repoName))
  // /<group-prefix>/chronosync - multicast
  , m_chronoSyncPrefix(Name("chronosync"))
  , m_face(m_ioService)
  , m_scheduler(m_ioService)
  // /localhost/repo1/datastream/insert
  , m_subscriber(m_face, Name("localhost").append("datastream").append(repoName).append("insert"))
  , m_cmdSigner(m_keyChain)
  {
    Name systemInfoName(pubSubGroupPrefix);
    systemInfoName.append("systemInfo");
    m_face.setInterestFilter(systemInfoName.toUri(),
			     bind(&ChronoApp::onSystemInfoInterest, this, _1, _2),
			     RegisterPrefixSuccessCallback(),
			     bind(&ChronoApp::onRegisterFailed, this, _1, _2));
    std::cout << "Registered prefix " << systemInfoName.toUri() << std::endl;
  }

  ~ChronoApp() {
    m_connection.disconnect();
  }

public:
  void run()
  {
    initializeRepoNamesAndDeviceNames();

    auto i = m_repoInfo.begin();
    for (i; i != m_repoInfo.end(); i++) {
      std::cout << "Repo name: " << i->first << std::endl;

      auto j = i->second.begin();
      for (j; j != i->second.end(); j++) {
	std::cout << *j << std::endl;
      }
    }
    
    initializeSyncRepo();

    try {
      m_face.processEvents();
    } catch (std::runtime_error& e) {
      std::cerr << e.what() << std::endl;
      return;
    }
  }

protected:
  void initializeSyncRepo()
  {
    m_chronoSyncSocket =
      std::make_shared<chronosync::Socket>(m_chronoSyncPrefix, m_chronoSyncUserPrefix, m_face,
                                           std::bind(&ChronoApp::onChronoSyncUpdate, this, _1));

    m_connection = m_subscriber.onNotification.connect(std::bind(&ChronoApp::onUpdateFromRepo, this, _1));
    m_subscriber.start();
  }

  void
  onChronoSyncUpdate(const std::vector<chronosync::MissingDataInfo>& v) {
    for (auto ms : v) {
      std::string prefix = ms.session.getPrefix(-1).toUri();

      // Check for our own update like NLSR
      if (prefix == m_chronoSyncUserPrefix.toUri()) {
        continue;
      }
  
      int seq1 = ms.low;
      int seq2 = ms.high;

      for (int i = seq1; i <= seq2; i++) {
        _LOG_INFO("ChronoSync Update: " << prefix << "/" << i);
	std::cout << "ChronoSync update: " << ms.session.toUri() << std::endl;
        fetchData(ms.session, i, 3);
      }
    }
  }

  void
  fetchData(const Name& sessionName, const uint64_t& seqNo,
            int nRetries)
  {
    Name interestName;
    interestName.append(sessionName).appendNumber(seqNo);

    Interest interest(interestName);
    //interest.setMustBeFresh(true);
    _LOG_INFO("Fetching data for : " << interest.getName() << " " << interest.getNonce());
    std::cout << "Fetching data for: " << interest.getName() << " " << interest.getNonce() << std::endl;
    
    m_face.expressInterest(interest,
                           std::bind(&ChronoApp::onData, this, _1, _2),
                           std::bind(&ChronoApp::onNack, this, _1, _2, nRetries),
                           std::bind(&ChronoApp::onTimeout, this, _1, nRetries));
  }

  void
  onData(const Interest& interest, const Data& data)
  {
    Name ds;
    // Content is the name
    ds.wireDecode(data.getContent().blockFromValue());

    // Get seqNumber from the data Name
    uint32_t seq = ds.get(ds.size()-1).toNumber();

    _LOG_INFO("ChronoSync DS Update: " << ds.getPrefix(-1) << "/" << seq);
    std::cout << "ChronoSync DS Update: " << ds << std::endl;
    insertIntoRepo(ds);
  }

  void
  onNack(const Interest& interest, const lp::Nack& nack, int nRetries)
  {
    _LOG_INFO("Nack: " << interest.getName() << " " << interest.getNonce());
    std::cout << "Nack: " << interest.getName() << " " << interest.getNonce() << std::endl;
  }
 
  void
  onTimeout(const Interest& interest, int nRetries)
  {
    _LOG_INFO("Timeout for interest: " << interest.getName() << " " << interest.getNonce());
    std::cout << "Timeout for interest: " << interest.getName() << " " << interest.getNonce() << std::endl;
    if (nRetries <= 0)
      return;

    Interest newNonceInterest(interest);
    newNonceInterest.refreshNonce();

    m_face.expressInterest(newNonceInterest,
                           std::bind(&ChronoApp::onData, this, _1, _2),
                           std::bind(&ChronoApp::onNack, this, _1, _2, nRetries-1),
                           std::bind(&ChronoApp::onTimeout, this, _1, nRetries-1)
                           );
  }

  void
  onSystemInfoInterest(const InterestFilter& filter, const Interest& interest)
  {
    std::cout << "Got system info interest." << std::endl;

    // Create new name, based on Interest's name
    Name dataName(interest.getName());

    std::string systemInfoString = "";

    auto i = m_repoInfo.begin();
    for (i; i != m_repoInfo.end(); i++) {
      std::cout << "In generate system info, on repo: " << i->first << std::endl;
      systemInfoString += i->first;
      systemInfoString += "\n";
      auto j = i->second.begin();
      for (j; j != i->second.end(); j++) {
	std::cout << "In generate system info, on device: " << *j << std::endl;
	systemInfoString += " " + *j + "\n";
      }
    }

    std::cout << "Generated system info: " << std::endl;
    std::cout << systemInfoString << std::endl;

    static const std::string content = systemInfoString;
    
    // Create Data packet
    shared_ptr<Data> data = make_shared<Data>();
    data->setName(dataName);
    data->setFreshnessPeriod(25_ms); // 10 seconds
    data->setContent(reinterpret_cast<const uint8_t*>(content.data()), content.size());
    
    // Sign Data packet with default identity
    m_keyChain.sign(*data);
    // m_keyChain.sign(data, <identityName>);
    // m_keyChain.sign(data, <certificate>);
    
    m_face.put(*data);
  }

  std::string generateSystemInfo() {
    std::string systemInfoString = "";

    auto i = m_repoInfo.begin();
    for (i; i != m_repoInfo.end(); i++) {
      std::cout << "In generate system info, on repo: " << i->first << std::endl;
      systemInfoString += i->first;
      systemInfoString += "\n";
      auto j = i->second.begin();
      for (j; j != i->second.end(); j++) {
	std::cout << "In generate system info, on device: " << *j << std::endl;
	systemInfoString += " " + *j + "\n";
      }
    }

    std::cout << "Generated system info: " << std::endl;
    std::cout << systemInfoString << std::endl;

    return systemInfoString;
  }
  
  void
  onRegisterFailed(const Name& prefix, const std::string& reason)
  {
    std::cerr << "ERROR: Failed to register prefix \""
	      << prefix << "\" in local hub's daemon (" << reason << ")"
	      << std::endl;
  }

  void
  insertIntoRepo(const Name& dataName) {
    /*
    try {
      std::string dataType = dataName.get(-3).toUri();
      if (dataType == "temperature" || dataType == "light" || dataType == "humidity" || dataType == "pressure" || dataType == "resistance") {}
      else {
	std::cout << "Data name's third from last component was: " << dataName.get(-3).toUri() << ", returning..." << std::endl;
        return;
      }
    } catch (const std::exception& e) {
      std::cout << e.what() << std::endl;
      return;
    }
    */

    _LOG_INFO("Inserting data into repo: " << dataName);
    std::cout << "Inserting data into repo: " << dataName << std::endl;

    //nameVector.push_back(dataName);

    RepoCommandParameter parameters;
    parameters.setName(dataName);

    // Generate command interest
    Interest interest;
    Name cmd(ndn::Name("localhost").append(m_repoName));
    cmd
    .append("insert")
    .append(parameters.wireEncode());

    interest = m_cmdSigner.makeCommandInterest(cmd);

    interest.setInterestLifetime(time::milliseconds(4000));

    m_face.expressInterest(interest,
                           nullptr,
                           std::bind(&ChronoApp::onRepoNack, this, _1, _2),
                           std::bind(&ChronoApp::onRepoTimeout, this, _1));

  }

    void
  onRepoNack(const Interest& interest, const lp::Nack& nack)
  {
    _LOG_INFO("Nack: " << interest.getName() << " " << interest.getNonce());
    std::cout << "Nack: " << interest.getName() << " " << interest.getNonce() << std::endl;
  }
 
  void
  onRepoTimeout(const Interest& interest)
  {
    _LOG_INFO("Timeout for interest: " << interest.getName() << " " << interest.getNonce());
    std::cout << "Timeout for interest: " << interest.getName() << " " << interest.getNonce() << std::endl;
  }


  void
  onUpdateFromRepo(const Data& data) {
    // Repo makes sure it streams only its own update
    Name comp = data.getName();
    std::cout << comp << std::endl;

    updateRepoInfo(comp);
    
    for (size_t i = 0; i < comp.size(); i++) {
      if (comp.at(i) == m_repoName.at(0)) {
        // update is from our own repo, safe to publish
        _LOG_INFO("ChronoSync Publish: " << data.getName());
	std::cout << "ChronoSync publish: " << data.getName() << std::endl;
	
        m_chronoSyncSocket->publishData(data.getName().wireEncode(),
                                        time::milliseconds(1000),
                                        m_chronoSyncUserPrefix);
        return;
      }
    }
    std::cout << "Got an update from repo about data we got from another repo, not publishing." << std::endl;

  }

  void updateRepoInfo(Name dataName) {
    // assumes that names will be formatted like this:
    // <pubsub prefix>/<repo name>/<device name>/<read type>/<time stamp>/<seq num>
    std::string repoName = dataName.get(-5).toUri();
    std::string deviceName = dataName.get(-4).toUri();

    std::cout << "Got chronosync update for repo " << repoName << ", device " << deviceName << std::endl;
    
    auto findRepo = m_repoInfo.find(repoName);
    
    if (findRepo == m_repoInfo.end()) {
      std::cout << "Got a chronosync update for a repo that we have not seen before, adding it to our list of repos." << std::endl;
      systemInfoWriteMutex.lock();
      insertNewRepoToRepoInfo(repoName);
      insertNewDeviceToRepoInfo(repoName, deviceName);
      systemInfoWriteMutex.unlock();
    }
    else if (findRepo->second.find(deviceName) == findRepo->second.end()) {
      std::cout << "Got a chronosync update for a device that we have not seen before, adding it to our list of devices." << std::endl;
      systemInfoWriteMutex.lock();
      insertNewDeviceToRepoInfo(repoName, deviceName);
      systemInfoWriteMutex.unlock();
    }
  }
  
  void initializeRepoNamesAndDeviceNames() {
    std::string repoListPath = m_systemInfoPath + "/repoList.txt";
    std::ifstream repoListFile(repoListPath.c_str());
    if (repoListFile.good()) {
      std::string repoName;
      std::string deviceName;
      while (std::getline(repoListFile, repoName))
	{
	  m_repoInfo.emplace(repoName, std::unordered_set<std::string>());
	  
	  auto repoEntry = m_repoInfo.find(repoName);
	  
	  std::string repoDeviceListFileName = m_systemInfoPath + "/" + repoName + ".txt";
	  
	  //need code here to load the contents of a file with that repo name, or create one if it doesn't exist
	  std::ifstream repoDeviceListFile(repoDeviceListFileName.c_str());
	  
	  if (repoDeviceListFile.good()) {
	    while (std::getline(repoDeviceListFile, deviceName)) {
	      repoEntry->second.insert(deviceName);
	    }
	  }
	  else {
	    std::ofstream outfile (m_systemInfoPath + "/" + repoName + ".txt");
	    outfile.close();
	  }
	  
	  repoDeviceListFile.close();
	}
    }
    else {
      std::ofstream out(repoListPath);
      out.close();
    }
    
    repoListFile.close();
  }
    
  
  void insertNewRepoToRepoInfo(std::string repoName) {
    std::string repoListPath = m_systemInfoPath + "/repoList.txt"; 
    auto find = m_repoInfo.find(repoName);
    if (find != m_repoInfo.end()) {
      std::cout << "The repo you were trying to insert was already in the repo list." << std::endl;
      return;
    }
    
    std::ofstream out;
    
    // std::ios::app is the open mode "append" meaning
    // new data will be written to the end of the file.
    out.open(repoListPath.c_str(), std::ios::app);
    
    if (out.good()) {
      out << repoName;
      out << "\n";
      
      out.close();
    }
    else {
      std::cout << "Failed to open repo list file for writing." << std::endl;
    }
    
    m_repoInfo.emplace(repoName, std::unordered_set<std::string>());
    
    std::ofstream outfile (m_systemInfoPath + "/" + repoName + ".txt");
    outfile.close();
    
  }
  
  void insertNewDeviceToRepoInfo(std::string repoName, std::string deviceName) {
    
    auto find = m_repoInfo.find(repoName);
    if (find == m_repoInfo.end()) {
      std::cout << "The repo that you are trying to insert a device for was not found." << std::endl;
      return;
    }
    
    auto findDevice = find->second.find(deviceName);
    if (findDevice != find->second.end()) {
      std::cout << "The device you were trying to insert was already recorded for that repo." << std::endl;
      return;
    }
    
    find->second.insert(deviceName);

    auto it = find->second.begin();
    std::cout << "Device names attached to repo " << repoName << std::endl;
    for (;it != find->second.end(); it++) {
      std::cout << *it << std::endl;
    }
    
    std::string repoDeviceListFileName = m_systemInfoPath + "/" + repoName + ".txt";
    
    std::ifstream repoDeviceListFile(repoDeviceListFileName.c_str());
    
    if (repoDeviceListFile.good()) {
      
    }
    else {
      std::ofstream outfile (m_systemInfoPath + "/" + repoName + ".txt");
      outfile.close();
    }
    
    repoDeviceListFile.close(); 
    
    std::ofstream out;
    
    out.open(repoDeviceListFileName.c_str(), std::ios::app);
    
    if (out.good()) {
      out << deviceName;
      out << "\n";
      
      out.close();
    }
    else {
      std::cout << "Failed to open repo's device list file for writing." << std::endl;
    }
  }
  
protected:
  Name m_pubSubGroupPrefix;
  Name m_repoName;

  Name m_chronoSyncUserPrefix;
  Name m_chronoSyncPrefix;

  boost::asio::io_service m_ioService;
  Face m_face;
  util::Scheduler m_scheduler;
  KeyChain m_keyChain;

  std::shared_ptr<chronosync::Socket> m_chronoSyncSocket;

  // Notification from Repo
  util::NotificationSubscriber<Data> m_subscriber;
  util::signal::Connection m_connection;
  std::vector<Name> nameVector;
  ndn::security::CommandInterestSigner m_cmdSigner;

  std::string m_systemInfoPath = "/home/pi/repo-ng/systemInfo";

  std::unordered_map<std::string, typename std::unordered_set<std::string>> m_repoInfo;

};

int main(int argc, char* argv[]) {
  if ( argc != 3 ) {
    std::cout << " Usage: " << argv[0]
              << " <pub-sub group prefix> <repoName>\n";
  }
  else {
    Name group(argv[1]);
    ChronoApp ChronoApp(group, Name(argv[2]));
    ChronoApp.run();
  }
}
