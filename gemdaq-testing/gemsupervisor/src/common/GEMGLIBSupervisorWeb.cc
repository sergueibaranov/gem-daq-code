#include<unistd.h>
#include "gem/supervisor/GEMGLIBSupervisorWeb.h"
#include "gem/readout/GEMDataParker.h"

#include "gem/hw/vfat/HwVFAT2.h"
#include "gem/hw/glib/HwGLIB.h"
#include "gem/hw/optohybrid/HwOptoHybrid.h"

#include <iomanip>
#include <iostream>
#include <ctime>
#include <sstream>
#include <cstdlib>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

XDAQ_INSTANTIATOR_IMPL(gem::supervisor::GEMGLIBSupervisorWeb)

void gem::supervisor::GEMGLIBSupervisorWeb::ConfigParams::registerFields(xdata::Bag<ConfigParams> *bag)
{
  latency   = 25U;

  outFileName  = "";
  outputType   = "Hex";

  for (int i = 0; i < 24; ++i) {
    deviceName.push_back("");
    deviceNum.push_back(-1);
  }

  triggerSource = 0x0;
  deviceChipID  = 0x0; 
  //can't assume a single value for all chips
  deviceVT1     = 0x0; 
  deviceVT2     = 0x0; 

  bag->addField("latency",       &latency );
  bag->addField("outputType",    &outputType  );
  bag->addField("outFileName",   &outFileName );

  bag->addField("deviceName",    &deviceName );
  bag->addField("deviceNum",     &deviceNum  );

  bag->addField("deviceIP",      &deviceIP    );
  bag->addField("triggerSource", &triggerSource );
  bag->addField("deviceChipID",  &deviceChipID  );
  bag->addField("deviceVT1",     &deviceVT1   );
  bag->addField("deviceVT2",     &deviceVT2   );

}

// Main constructor
gem::supervisor::GEMGLIBSupervisorWeb::GEMGLIBSupervisorWeb(xdaq::ApplicationStub * s):
  xdaq::WebApplication(s),
  m_gemLogger(this->getApplicationLogger()),
  wl_semaphore_(toolbox::BSem::FULL),
  hw_semaphore_(toolbox::BSem::FULL),
  //readout_mask, as it is currently implemented, is not sensible in the V2 firmware
  //can consider using this as the tracking/broadcast mask (initializing to 0xffffffff (everything masked off)
  //readout_mask(0xffffffff),
  readout_mask(0x0),
  is_working_ (false),
  is_initialized_ (false),
  is_configured_ (false),
  is_running_ (false)
{
  // Detect when the setting of default parameters has been performed
  this->getApplicationInfoSpace()->addListener(this, "urn:xdaq-event:setDefaultValues");

  getApplicationInfoSpace()->fireItemAvailable("confParams", &confParams_);
  getApplicationInfoSpace()->fireItemValueRetrieve("confParams", &confParams_);

  // HyperDAQ bindings
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webDefault,     "Default"    );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webConfigure,   "Configure"  );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webStart,       "Start"      );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webStop,        "Stop"       );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webHalt,        "Halt"       );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webTrigger,     "Trigger"    );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webL1ACalPulse, "L1ACalPulse");
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webResync,      "Resync"     );
  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::webBC0,         "BC0"        );

  xgi::framework::deferredbind(this, this, &gem::supervisor::GEMGLIBSupervisorWeb::setParameter,   "setParameter");

  // SOAP bindings
  xoap::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::onConfigure, "Configure", XDAQ_NS_URI);
  xoap::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::onStart,     "Start",     XDAQ_NS_URI);
  xoap::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::onStop,      "Stop",      XDAQ_NS_URI);
  xoap::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::onHalt,      "Halt",      XDAQ_NS_URI);

  // Initiate and activate main workloop
  wl_ = toolbox::task::getWorkLoopFactory()->getWorkLoop("GEMGLIBSupervisorWebWorkLoop", "waiting");
  wl_->activate();

  // Workloop bindings
  configure_signature_ = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::configureAction, "configureAction");
  start_signature_     = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::startAction,     "startAction"    );
  stop_signature_      = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::stopAction,      "stopAction"     );
  halt_signature_      = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::haltAction,      "haltAction"     );
  run_signature_       = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::runAction,       "runAction"      );
  read_signature_      = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::readAction,      "readAction"     );
  select_signature_    = toolbox::task::bind(this, &gem::supervisor::GEMGLIBSupervisorWeb::selectAction,    "selectAction"   );

  // Define FSM states
  fsm_.addState('I', "Initial",    this, &gem::supervisor::GEMGLIBSupervisorWeb::stateChanged);
  fsm_.addState('H', "Halted",     this, &gem::supervisor::GEMGLIBSupervisorWeb::stateChanged);
  fsm_.addState('C', "Configured", this, &gem::supervisor::GEMGLIBSupervisorWeb::stateChanged);
  fsm_.addState('R', "Running",    this, &gem::supervisor::GEMGLIBSupervisorWeb::stateChanged);

  // Define error FSM state
  fsm_.setStateName('F', "Error");
  fsm_.setFailedStateTransitionAction(this, &gem::supervisor::GEMGLIBSupervisorWeb::transitionFailed);
  fsm_.setFailedStateTransitionChanged(this, &gem::supervisor::GEMGLIBSupervisorWeb::stateChanged);

  // Define allowed FSM state transitions
  fsm_.addStateTransition('H', 'C', "Configure", this, &gem::supervisor::GEMGLIBSupervisorWeb::configureAction);
  fsm_.addStateTransition('H', 'H', "Halt",      this, &gem::supervisor::GEMGLIBSupervisorWeb::haltAction);
  fsm_.addStateTransition('C', 'C', "Configure", this, &gem::supervisor::GEMGLIBSupervisorWeb::configureAction);
  fsm_.addStateTransition('C', 'R', "Start",     this, &gem::supervisor::GEMGLIBSupervisorWeb::startAction);
  fsm_.addStateTransition('C', 'H', "Halt",      this, &gem::supervisor::GEMGLIBSupervisorWeb::haltAction);
  fsm_.addStateTransition('R', 'C', "Stop",      this, &gem::supervisor::GEMGLIBSupervisorWeb::stopAction);
  fsm_.addStateTransition('R', 'H', "Halt",      this, &gem::supervisor::GEMGLIBSupervisorWeb::haltAction);

  // Define forbidden FSM state transitions
  fsm_.addStateTransition('R', 'R', "Configure" , this, &gem::supervisor::GEMGLIBSupervisorWeb::noAction);
  fsm_.addStateTransition('H', 'H', "Start"     , this, &gem::supervisor::GEMGLIBSupervisorWeb::noAction);
  fsm_.addStateTransition('R', 'R', "Start"     , this, &gem::supervisor::GEMGLIBSupervisorWeb::noAction);
  fsm_.addStateTransition('H', 'H', "Stop"      , this, &gem::supervisor::GEMGLIBSupervisorWeb::noAction);
  fsm_.addStateTransition('C', 'C', "Stop"      , this, &gem::supervisor::GEMGLIBSupervisorWeb::noAction);

  // Set initial FSM state and reset FSM
  fsm_.setInitialState('H');
  fsm_.reset();

  counter_ = {0,0,0};

}

void gem::supervisor::GEMGLIBSupervisorWeb::actionPerformed(xdata::Event& event)
{
  // This is called after all default configuration values have been
  // loaded (from the XDAQ configuration file).
  if (event.type() == "urn:xdaq-event:setDefaultValues") {
    std::stringstream ss;
    ss << "deviceIP=["    << confParams_.bag.deviceIP.toString()    << "]" << std::endl;
    ss << "outFileName=[" << confParams_.bag.outFileName.toString() << "]" << std::endl;
    ss << "outputType=["  << confParams_.bag.outputType.toString()  << "]" << std::endl;
    ss << "latency=["     << confParams_.bag.latency.toString()     << "]" << std::endl;
    ss << "triggerSource=[" << confParams_.bag.triggerSource.toString() << "]" << std::endl;
    ss << "deviceChipID=["  << confParams_.bag.deviceChipID.toString()  << "]" << std::endl;
    ss << "deviceVT1=[" << confParams_.bag.deviceVT1.toString() << "]" << std::endl;
    ss << "deviceVT2=[" << confParams_.bag.deviceVT2.toString() << "]" << std::endl;

    auto num = confParams_.bag.deviceNum.begin();
    for (auto chip = confParams_.bag.deviceName.begin();
      chip != confParams_.bag.deviceName.end(); ++chip, ++num) {
        ss << "Device name: " << chip->toString() << std::endl;
      }
    INFO(ss.str());
  }

}

xoap::MessageReference gem::supervisor::GEMGLIBSupervisorWeb::onConfigure(xoap::MessageReference message) {
  is_working_ = true;

  wl_->submit(configure_signature_);
  return message;
}

xoap::MessageReference gem::supervisor::GEMGLIBSupervisorWeb::onStart(xoap::MessageReference message) {
  is_working_ = true;

  wl_->submit(start_signature_);
  return message;
}

xoap::MessageReference gem::supervisor::GEMGLIBSupervisorWeb::onStop(xoap::MessageReference message) {
  is_working_ = true;

  wl_->submit(stop_signature_);
  return message;
}

xoap::MessageReference gem::supervisor::GEMGLIBSupervisorWeb::onHalt(xoap::MessageReference message) {
  is_working_ = true;

  wl_->submit(halt_signature_);
  return message;
}

// HyperDAQ interface
void gem::supervisor::GEMGLIBSupervisorWeb::webDefault(xgi::Input * in, xgi::Output * out ) {
  // Define how often main web interface refreshes
  if (!is_working_ && !is_running_) {
  }
  else if (is_working_) {
    cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
    head.addHeader("Refresh","3");
  }
  else if (is_running_) {
    cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
    head.addHeader("Refresh","3");
  }

  if (is_configured_) {
    //counting "1" Internal triggers, one link enough 
    m_l1aCount[0] = optohybridDevice_->getL1ACount(0); //ttc
    m_l1aCount[1] = optohybridDevice_->getL1ACount(1); //internal/firmware
    m_l1aCount[2] = optohybridDevice_->getL1ACount(2); //external
    m_l1aCount[3] = optohybridDevice_->getL1ACount(3); //loopback 
    m_l1aCount[4] = optohybridDevice_->getL1ACount(4); //sent
   
    m_calPulseCount[0] = optohybridDevice_->getCalPulseCount(0); //ttc
    m_calPulseCount[1] = optohybridDevice_->getCalPulseCount(1); //internal/firmware
    m_calPulseCount[2] = optohybridDevice_->getCalPulseCount(2); //external
    m_calPulseCount[3] = optohybridDevice_->getCalPulseCount(3); //loopback 
    m_calPulseCount[4] = optohybridDevice_->getCalPulseCount(4); //sent
    
    m_resyncCount[0] = optohybridDevice_->getResyncCount(0); //ttc
    m_resyncCount[1] = optohybridDevice_->getResyncCount(1); //internal/firmware
    m_resyncCount[2] = optohybridDevice_->getResyncCount(2); //external
    m_resyncCount[3] = optohybridDevice_->getResyncCount(3); //loopback 
    m_resyncCount[4] = optohybridDevice_->getResyncCount(4); //sent
    
    m_bc0Count[0] = optohybridDevice_->getBC0Count(0); //ttc
    m_bc0Count[1] = optohybridDevice_->getBC0Count(1); //internal/firmware
    m_bc0Count[2] = optohybridDevice_->getBC0Count(2); //external
    m_bc0Count[3] = optohybridDevice_->getBC0Count(3); //loopback 
    m_bc0Count[4] = optohybridDevice_->getBC0Count(4); //sent
  }

  // Page title
  *out << cgicc::h1("GEM DAQ Supervisor")<< std::endl;

  // Choose DAQ type: Spy or Global
  *out << "DAQ type: " << cgicc::select().set("name", "runtype");
  *out << cgicc::option().set("value", "Spy").set("selected","") << "Spy" << cgicc::option();
  *out << cgicc::option().set("value", "Global") << "Global" << cgicc::option();
  *out << cgicc::select() << std::endl;
  *out << cgicc::input().set("type", "submit").set("name", "command").set("title", "Set DAQ type").set("value", "Set DAQ type") 
       << cgicc::br() << cgicc::br();

  *out << cgicc::fieldset().set("style","font-size: 10pt;  font-family: arial;") << std::endl;
  std::string method = toolbox::toString("/%s/setParameter",getApplicationDescriptor()->getURN().c_str());
  *out << cgicc::legend("Set Hex/Binary of output") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", method) << std::endl;
  *out << cgicc::input().set("type","text").set("name","value").set("value", confParams_.bag.outputType.toString())   << std::endl;
  *out << cgicc::input().set("type","submit").set("value","Apply")  << std::endl;
  *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // Show current state, counter, output filename
  std::string theState = fsm_.getStateName(fsm_.getCurrentState());
  *out << "Current state: "       << theState                          << cgicc::br() << std::endl;
  *out << "Event counter: "       << counter_[1]  << " Events counter" << cgicc::br() << std::endl;
  //*out << "<table class=\"xdaq-table\">" << std::endl
  *out << cgicc::table().set("class", "xdaq-table") << std::endl
       << cgicc::thead() << std::endl
       << cgicc::tr()    << std::endl //open
       << cgicc::th()    << "T1 counters" << cgicc::th() << std::endl
       << cgicc::th()    << "TTC"         << cgicc::th() << std::endl
       << cgicc::th()    << "Firmware"    << cgicc::th() << std::endl
       << cgicc::th()    << "External"    << cgicc::th() << std::endl
       << cgicc::th()    << "Loopback"    << cgicc::th() << std::endl
       << cgicc::th()    << "Sent"        << cgicc::th() << std::endl
       << cgicc::tr()    << std::endl //close
       << cgicc::thead() << std::endl 
    
       << cgicc::tbody() << std::endl 
       << cgicc::br()    << std::endl;
  *out << cgicc::tr() << std::endl
       << cgicc::td() << "L1A:"        << cgicc::td() << std::endl
       << cgicc::td() << m_l1aCount[0] << cgicc::td() << std::endl
       << cgicc::td() << m_l1aCount[1] << cgicc::td() << std::endl
       << cgicc::td() << m_l1aCount[2] << cgicc::td() << std::endl
       << cgicc::td() << m_l1aCount[3] << cgicc::td() << std::endl
       << cgicc::td() << m_l1aCount[4] << cgicc::td() << std::endl
       << cgicc::tr() << cgicc::br() << std::endl;
  *out << cgicc::tr() << std::endl
       << cgicc::td() << "CalPulse:"        << cgicc::td() << std::endl
       << cgicc::td() << m_calPulseCount[0] << cgicc::td() << std::endl
       << cgicc::td() << m_calPulseCount[1] << cgicc::td() << std::endl
       << cgicc::td() << m_calPulseCount[2] << cgicc::td() << std::endl
       << cgicc::td() << m_calPulseCount[3] << cgicc::td() << std::endl
       << cgicc::td() << m_calPulseCount[4] << cgicc::td() << std::endl
       << cgicc::tr() << cgicc::br() << std::endl;
  *out << cgicc::tr() << std::endl
       << cgicc::td() << "Resync:"        << cgicc::td() << std::endl
       << cgicc::td() << m_resyncCount[0] << cgicc::td() << std::endl
       << cgicc::td() << m_resyncCount[1] << cgicc::td() << std::endl
       << cgicc::td() << m_resyncCount[2] << cgicc::td() << std::endl
       << cgicc::td() << m_resyncCount[3] << cgicc::td() << std::endl
       << cgicc::td() << m_resyncCount[4] << cgicc::td() << std::endl
       << cgicc::tr() << cgicc::br() << std::endl;
  *out << cgicc::tr() << std::endl
       << cgicc::td() << "BC0:"        << cgicc::td() << std::endl
       << cgicc::td() << m_bc0Count[0] << cgicc::td() << std::endl
       << cgicc::td() << m_bc0Count[1] << cgicc::td() << std::endl
       << cgicc::td() << m_bc0Count[2] << cgicc::td() << std::endl
       << cgicc::td() << m_bc0Count[3] << cgicc::td() << std::endl
       << cgicc::td() << m_bc0Count[4] << cgicc::td() << std::endl
       << cgicc::tr() << cgicc::br() << std::endl
       << cgicc::tbody() << std::endl << cgicc::br()
       << cgicc::table() << std::endl << cgicc::br();
  *out << "VFAT blocks counter: "       << counter_[0] << " dumped to disk" << std::endl << cgicc::br();
  *out << "VFATs counter, last event: " << counter_[2] << " VFATs chips"    << std::endl << cgicc::br();
  *out << "Output filename: " << confParams_.bag.outFileName.toString() << std::endl << cgicc::br();
  *out << "Output type: "     << confParams_.bag.outputType.toString()  << std::endl << cgicc::br();

  // Table with action buttons
  *out << cgicc::table().set("border","0");

  // Row with action buttons
  *out << cgicc::tr();
  if (!is_working_) {
    if (!is_configured_) {
      // Configure button
      *out << cgicc::td();
      std::string configureButton = toolbox::toString("/%s/Configure",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",configureButton) << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Configure")    << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    } else {
      if (!is_running_) {
        // Start button
        *out << cgicc::td();
        std::string startButton = toolbox::toString("/%s/Start",getApplicationDescriptor()->getURN().c_str());
        *out << cgicc::form().set("method","GET").set("action",startButton) << std::endl;
        *out << cgicc::input().set("type","submit").set("value","Start")    << std::endl;
        *out << cgicc::form();
        *out << cgicc::td();
      } else {
        // Stop button
        *out << cgicc::td();
        std::string stopButton = toolbox::toString("/%s/Stop",getApplicationDescriptor()->getURN().c_str());
        *out << cgicc::form().set("method","GET").set("action",stopButton) << std::endl;
        *out << cgicc::input().set("type","submit").set("value","Stop")    << std::endl;
        *out << cgicc::form();
        *out << cgicc::td();
      }
      // Halt button
      *out << cgicc::td();
      std::string haltButton = toolbox::toString("/%s/Halt",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",haltButton) << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Halt")    << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    
      // Send L1A signal
      *out << cgicc::td();
      std::string triggerButton = toolbox::toString("/%s/Trigger",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",triggerButton) << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Send L1A")   << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    
      // Send L1ACalPulse signal
      *out << cgicc::td();
      std::string calpulseButton = toolbox::toString("/%s/L1ACalPulse",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",calpulseButton)      << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Send L1ACalPulse") << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    
      // Send Resync signal
      *out << cgicc::td();
      std::string resyncButton = toolbox::toString("/%s/Resync",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",resyncButton)   << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Send Resync") << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    
      // Send BC0 signal
      *out << cgicc::td();
      std::string bc0Button = toolbox::toString("/%s/BC0",getApplicationDescriptor()->getURN().c_str());
      *out << cgicc::form().set("method","GET").set("action",bc0Button)   << std::endl;
      *out << cgicc::input().set("type","submit").set("value","Send BC0") << std::endl;
      *out << cgicc::form();
      *out << cgicc::td();
    }// end is_configured
  }//end is_working
  // Finish row with action buttons
  *out << cgicc::tr();

  // Finish table with action buttons
  *out << cgicc::table();

}

void gem::supervisor::GEMGLIBSupervisorWeb::setParameter(xgi::Input * in, xgi::Output * out ) {
  try{
    cgicc::Cgicc cgi(in);
    confParams_.bag.outputType = cgi["value"]->getValue();

    // re-display form page 
    this->webDefault(in,out);		
  }
  catch (const std::exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }	
}

void gem::supervisor::GEMGLIBSupervisorWeb::webConfigure(xgi::Input * in, xgi::Output * out ) {
  // Derive device number from device name

  int islot = 0;
  for (auto chip = confParams_.bag.deviceName.begin(); chip != confParams_.bag.deviceName.end(); ++chip, ++islot ) {
    std::string VfatName = chip->toString();
    if (VfatName != ""){ 
      if ( islot >= 0 ) {
        //readout_mask, as it is currently implemented, is not sensible in the V2 firmware
        //can consider using this as the tracking/broadcast mask (initializing to 0xffffffff (everything masked off)
        //readout_mask &= (0xffffffff & 0x0 <<;
        readout_mask |= 0x1 << islot;
        INFO(" webConfigure : DeviceName " << VfatName );
        INFO(" webConfigure : readout_mask 0x" << std::hex << (int)readout_mask << std::dec );
      }
    }//end if VfatName
  }//end for chip
  //hard code the readout mask for now, since this readout mask is an artifact of V1.5 /**JS Oct 8*/
  readout_mask = ~readout_mask;
  INFO(" webConfigure : readout_mask 0x" << std::hex << (int)readout_mask << std::dec );
  readout_mask = 0x1;
  // Initiate configure workloop
  wl_->submit(configure_signature_);

  INFO(" webConfigure : readout_mask 0x" << std::hex << (int)readout_mask << std::dec);
  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webStart(xgi::Input * in, xgi::Output * out ) {
  // Initiate start workloop
  wl_->submit(start_signature_);
    
  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webStop(xgi::Input * in, xgi::Output * out ) {
  // Initiate stop workloop
  wl_->submit(stop_signature_);

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webHalt(xgi::Input * in, xgi::Output * out ) {
  // Initiate halt workloop
  wl_->submit(halt_signature_);

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webTrigger(xgi::Input * in, xgi::Output * out ) {
  // Send L1A signal
  hw_semaphore_.take();

  INFO(" webTrigger: sending L1A");
  optohybridDevice_->sendL1A(0, 20);

  m_l1aCount[0] = optohybridDevice_->getL1ACount(0); //ttc
  m_l1aCount[1] = optohybridDevice_->getL1ACount(1); //internal/firmware
  m_l1aCount[2] = optohybridDevice_->getL1ACount(2); //external
  m_l1aCount[3] = optohybridDevice_->getL1ACount(3); //loopback 
  m_l1aCount[4] = optohybridDevice_->getL1ACount(4); //sent
  
  hw_semaphore_.give();

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webL1ACalPulse(xgi::Input * in, xgi::Output * out ) {
  // Send L1A signal
  hw_semaphore_.take();
  for (int offset = -12; offset < 13; ++offset) {
    INFO("webCalPulse: sending 10 CalPulses with L1As delayed by " << (int)latency_ + offset <<  " clocks");
    optohybridDevice_->sendL1ACal(2, latency_ + offset);
  }
  m_calPulseCount[0] = optohybridDevice_->getCalPulseCount(0); //ttc
  m_calPulseCount[1] = optohybridDevice_->getCalPulseCount(1); //internal/firmware
  m_calPulseCount[2] = optohybridDevice_->getCalPulseCount(2); //external
  m_calPulseCount[3] = optohybridDevice_->getCalPulseCount(3); //loopback 
  m_calPulseCount[4] = optohybridDevice_->getCalPulseCount(4); //sent
  
  hw_semaphore_.give();

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webResync(xgi::Input * in, xgi::Output * out ) {
  // Send L1A signal
  hw_semaphore_.take();

  INFO("webResync: sending Resync");
  optohybridDevice_->sendResync();
  m_resyncCount[0] = optohybridDevice_->getResyncCount(0); //ttc
  m_resyncCount[1] = optohybridDevice_->getResyncCount(1); //internal/firmware
  m_resyncCount[2] = optohybridDevice_->getResyncCount(2); //external
  m_resyncCount[3] = optohybridDevice_->getResyncCount(3); //loopback 
  m_resyncCount[4] = optohybridDevice_->getResyncCount(4); //sent
  
  hw_semaphore_.give();

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webBC0(xgi::Input * in, xgi::Output * out ) {
  // Send L1A signal
  hw_semaphore_.take();

  INFO("webBC0: sending BC0");
  optohybridDevice_->sendBC0();
  m_bc0Count[0] = optohybridDevice_->getBC0Count(0); //ttc
  m_bc0Count[1] = optohybridDevice_->getBC0Count(1); //internal/firmware
  m_bc0Count[2] = optohybridDevice_->getBC0Count(2); //external
  m_bc0Count[3] = optohybridDevice_->getBC0Count(3); //loopback 
  m_bc0Count[4] = optohybridDevice_->getBC0Count(4); //sent
  
  hw_semaphore_.give();

  // Go back to main web interface
  this->webRedirect(in, out);
}

void gem::supervisor::GEMGLIBSupervisorWeb::webRedirect(xgi::Input *in, xgi::Output* out)  {
  // Redirect to main web interface
  std::string url = "/" + getApplicationDescriptor()->getURN() + "/Default";
  *out << "<meta http-equiv=\"refresh\" content=\"0;" << url << "\">" << std::endl;

  this->webDefault(in,out);
}

bool gem::supervisor::GEMGLIBSupervisorWeb::configureAction(toolbox::task::WorkLoop *wl)
{
  // fire "Configure" event to FSM
  fireEvent("Configure");

  optohybridDevice_->sendResync();

  return false;
}

bool gem::supervisor::GEMGLIBSupervisorWeb::startAction(toolbox::task::WorkLoop *wl)
{
  // fire "Start" event to FSM
  fireEvent("Start");
  return false;
}

bool gem::supervisor::GEMGLIBSupervisorWeb::stopAction(toolbox::task::WorkLoop *wl)
{
  // Fire "Stop" event to FSM
  fireEvent("Stop");
  return false;
}

bool gem::supervisor::GEMGLIBSupervisorWeb::haltAction(toolbox::task::WorkLoop *wl)
{
  // Fire "Halt" event to FSM
  fireEvent("Halt");
  return false;
}

bool gem::supervisor::GEMGLIBSupervisorWeb::runAction(toolbox::task::WorkLoop *wl)
{
  wl_semaphore_.take();
  hw_semaphore_.take();

  uint32_t bufferDepth = 0;
  bufferDepth = glibDevice_->getFIFOVFATBlockOccupancy(0x0);
  wl_semaphore_.give();
  hw_semaphore_.give();

  DEBUG("Combined bufferDepth = 0x" << std::hex << bufferDepth << std::dec);

  // If GLIB data buffer has non-zero size, initiate read workloop
  if (bufferDepth>3) {
    wl_->submit(read_signature_);
  }//end bufferDepth

  //should possibly return true so the workloop is automatically resubmitted
  return true;
}

bool gem::supervisor::GEMGLIBSupervisorWeb::readAction(toolbox::task::WorkLoop *wl)
{
  hw_semaphore_.take();

  uint32_t* pDupm = gemDataParker->dumpData(readout_mask);
  if (pDupm) {
    counter_[0] = *pDupm;     // VFAT Blocks counter
    counter_[1] = *(pDupm+1); // Events counter
    counter_[2] = *(pDupm+2); // Sum VFAT per last event
  }

  hw_semaphore_.give();

  //should possibly return true so the workloop is automatically resubmitted
  return false;
}


bool gem::supervisor::GEMGLIBSupervisorWeb::selectAction(toolbox::task::WorkLoop *wl)
{

  uint32_t  Counter[5] = {0,0,0,0,0};
  uint32_t* pDQ =  gemDataParker->selectData(Counter);
  if (pDQ) {
    Counter[0] = *(pDQ+0); // VFAT block counter
    Counter[1] = *(pDQ+1); // Events counter
    Counter[2] = *(pDQ+2); // Counter[3]+Counter[4], last event
    Counter[3] = *(pDQ+3); // good VFAT's blocks, last event
    Counter[4] = *(pDQ+4); // bad VFAT's block, last event 
  }

  if (is_running_) {return true;} else {return false;}
}


// State transitions
void gem::supervisor::GEMGLIBSupervisorWeb::configureAction(toolbox::Event::Reference evt) {
  is_working_ = true;
  hw_semaphore_.take();

  counter_ = {0,0,0};

  std::stringstream tmpURI;
  tmpURI << "chtcp-2.0://localhost:10203?target=" << confParams_.bag.deviceIP.toString() << ":50001";
  glibDevice_ = glib_shared_ptr(new gem::hw::glib::HwGLIB("HwGLIB", tmpURI.str(),
                                                          "file://${GEM_ADDRESS_TABLE_PATH}/glib_address_table.xml"));
  optohybridDevice_ = optohybrid_shared_ptr(new gem::hw::optohybrid::HwOptoHybrid("HwOptoHybrid0", tmpURI.str(),
                                                                                  "file://${GEM_ADDRESS_TABLE_PATH}/glib_address_table.xml"));
  INFO("setTrigSource OH mode 1");
  optohybridDevice_->setTrigSource(0x1);

  // Times for output files
  time_t now  = time(0);
  tm    *gmtm = gmtime(&now);
  char* utcTime = asctime(gmtm);

  // Setup file, information header
  std::string SetupFileName = "Setup_";
  SetupFileName.append(utcTime);
  SetupFileName.erase(std::remove(SetupFileName.begin(), SetupFileName.end(), '\n'), SetupFileName.end());
  SetupFileName.append(".txt");
  std::replace(SetupFileName.begin(), SetupFileName.end(), ' ', '_' );
  std::replace(SetupFileName.begin(), SetupFileName.end(), ':', '-');

  INFO("::configureAction Created Setup file " << SetupFileName );

  std::ofstream SetupFile(SetupFileName.c_str(), std::ios::app );
  if (SetupFile.is_open()){
    SetupFile << std::endl << "The Time & Date : " << utcTime << std::endl;
  }

  int islot=0;
  for (auto chip = confParams_.bag.deviceName.begin(); chip != confParams_.bag.deviceName.end(); ++chip, ++islot) {
    std::string VfatName = chip->toString();

    if (VfatName != ""){ 
      vfat_shared_ptr tmpVFATDevice(new gem::hw::vfat::HwVFAT2(VfatName, tmpURI.str(),
                                                               "file://${GEM_ADDRESS_TABLE_PATH}/glib_address_table.xml"));
      tmpVFATDevice->setDeviceIPAddress(confParams_.bag.deviceIP);
      tmpVFATDevice->setRunMode(0);
      // need to put all chips in sleep mode to start off
      vfatDevice_.push_back(tmpVFATDevice);
      }
  }
  
  islot=0;
  for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip, ++islot) {
    (*chip)->setDeviceIPAddress(confParams_.bag.deviceIP);
    (*chip)->readVFAT2Counters();
    (*chip)->setRunMode(0);

    confParams_.bag.deviceChipID = (*chip)->getChipID();
    
    latency_   = confParams_.bag.latency;
    deviceVT1_ = confParams_.bag.deviceVT1;

    // Set VFAT2 registers
    (*chip)->loadDefaults();
    (*chip)->setLatency(latency_);
    confParams_.bag.latency = (*chip)->getLatency();
    
    (*chip)->setVThreshold1(deviceVT1_);
    confParams_.bag.deviceVT1 = (*chip)->getVThreshold1();

    (*chip)->setVThreshold2(0);
    confParams_.bag.deviceVT2 = (*chip)->getVThreshold2();
  }

  // Create a new output file for Data flow
  std::string tmpFileName = "GEMDAQ_", tmpType = "";
  tmpFileName.append(utcTime);
  tmpFileName.erase(std::remove(tmpFileName.begin(), tmpFileName.end(), '\n'), tmpFileName.end());
  tmpFileName.append(".dat");
  std::replace(tmpFileName.begin(), tmpFileName.end(), ' ', '_' );
  std::replace(tmpFileName.begin(), tmpFileName.end(), ':', '-');

  std::string errFileName = "ERRORS_";
  errFileName.append(utcTime);
  errFileName.erase(std::remove(errFileName.begin(), errFileName.end(), '\n'), errFileName.end());
  errFileName.append(".dat");
  std::replace(errFileName.begin(), errFileName.end(), ' ', '_' );
  std::replace(errFileName.begin(), errFileName.end(), ':', '-');

  confParams_.bag.outFileName = tmpFileName;
  std::ofstream outf(tmpFileName.c_str(), std::ios_base::app | std::ios::binary );
  std::ofstream errf(errFileName.c_str(), std::ios_base::app | std::ios::binary );

  tmpType = confParams_.bag.outputType.toString();

  // Book GEM Data Parker
  gemDataParker = std::shared_ptr<gem::readout::GEMDataParker>(new 
                                  gem::readout::GEMDataParker(*glibDevice_, tmpFileName, errFileName, tmpType)
                                 );

  // Data Stream close
  outf.close();
  errf.close();

  if (SetupFile.is_open()){
    SetupFile << " Latency       " << latency_   << std::endl;
    SetupFile << " Threshold     " << deviceVT1_ << std::endl << std::endl;
  }
  ////this is not good!!!
  //hw_semaphore_.give();
  /** Super hacky, also doesn't work as the state is taken from the FSM rather
      than this parameter (as it should), J.S July 16
      Failure of any of the conditions at the moment does't take the FSM to error, should it? J.S. Sep 13
  */
  if (glibDevice_->isHwConnected()) {
    INFO("GLIB device connected");
    if (optohybridDevice_->isHwConnected()) {
      INFO("OptoHybrid device connected");
      for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip) {
        if ((*chip)->isHwConnected()) {
          INFO("VFAT device connected: chip ID = 0x"
               << std::setw(4) << std::setfill('0') << std::hex
               << (uint32_t)((*chip)->getChipID())  << std::dec);
          INFO((*chip)->printErrorCounts());

          int islot = gem::readout::GEMslotContents::GEBslotIndex( (uint32_t)((*chip)->getChipID()) );

          if (SetupFile.is_open()){
            SetupFile << " VFAT device connected: slot "
                      << std::setw(2) << std::setfill('0') << islot << " chip ID = 0x" 
                      << std::setw(3) << std::setfill('0') << std::hex
                      << (uint32_t)((*chip)->getChipID()) << std::dec << std::endl;
            (*chip)->printDefaults(SetupFile);
          }
          is_configured_  = true;
        } else {
          INFO("VFAT device not connected, breaking out");
          is_configured_  = false;
          is_working_     = false;    
          hw_semaphore_.give();
          // Setup header close, don't leave open file handles laying around
          SetupFile.close();
          return;
        }
      }
    } else {
      INFO("OptoHybrid device not connected, breaking out");
      is_configured_  = false;
      is_working_     = false;    
      hw_semaphore_.give();
      // Setup header close, don't leave open file handles laying around
      SetupFile.close();
      return;
    }
  } else {
    INFO("GLIB device not connected, breaking out");
    is_configured_  = false;
    is_working_     = false;    
    hw_semaphore_.give();
    // Setup header close, don't leave open file handles laying around
    SetupFile.close();
    return;
  }
  hw_semaphore_.give();

  // Setup header close
  SetupFile.close();

  //is_configured_  = true;
  is_working_     = false;    
  
}

void gem::supervisor::GEMGLIBSupervisorWeb::startAction(toolbox::Event::Reference evt) {
  is_working_ = true;

  is_running_ = true;

  hw_semaphore_.take();

  INFO("setTrigSource OH mode 0");
  optohybridDevice_->setTrigSource(0x0);

  INFO("Enabling run mode for selected VFATs");
  for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip)
    (*chip)->setRunMode(1);

  //flush FIFO, how to disable a specific, misbehaving, chip
  INFO("Flushing the FIFOs, readout_mask 0x" <<std::hex << (int)readout_mask << std::dec);
  for (int i = 0; i < 2; ++i) {
    DEBUG("Flushing FIFO" << i << " (depth " << glibDevice_->getFIFOOccupancy(i));
    if ((readout_mask >> i)&0x1) {
      DEBUG("Flushing FIFO" << i << " (depth " << glibDevice_->getFIFOOccupancy(i));
      glibDevice_->flushFIFO(i);
      while (glibDevice_->hasTrackingData(i)) {
        glibDevice_->flushFIFO(i);
        std::vector<uint32_t> dumping = glibDevice_->getTrackingData(i);
      }
      glibDevice_->flushFIFO(i);
    }
  }

  //send resync
  INFO("Sending a resync");
  optohybridDevice_->sendResync();

  //reset counters
  INFO("Resetting counters");
  optohybridDevice_->resetL1ACount(0x5);
  optohybridDevice_->resetResyncCount(0x5);
  optohybridDevice_->resetBC0Count(0x5);
  optohybridDevice_->resetCalPulseCount(0x5);

  m_l1aCount[0] = optohybridDevice_->getL1ACount(0); //ttc
  m_l1aCount[1] = optohybridDevice_->getL1ACount(1); //internal/firmware
  m_l1aCount[2] = optohybridDevice_->getL1ACount(2); //external
  m_l1aCount[3] = optohybridDevice_->getL1ACount(3); //loopback 
  m_l1aCount[4] = optohybridDevice_->getL1ACount(4); //sent
   
  m_calPulseCount[0] = optohybridDevice_->getCalPulseCount(0); //ttc
  m_calPulseCount[1] = optohybridDevice_->getCalPulseCount(1); //internal/firmware
  m_calPulseCount[2] = optohybridDevice_->getCalPulseCount(2); //external
  m_calPulseCount[3] = optohybridDevice_->getCalPulseCount(3); //loopback 
  m_calPulseCount[4] = optohybridDevice_->getCalPulseCount(4); //sent
    
  m_resyncCount[0] = optohybridDevice_->getResyncCount(0); //ttc
  m_resyncCount[1] = optohybridDevice_->getResyncCount(1); //internal/firmware
  m_resyncCount[2] = optohybridDevice_->getResyncCount(2); //external
  m_resyncCount[3] = optohybridDevice_->getResyncCount(3); //loopback 
  m_resyncCount[4] = optohybridDevice_->getResyncCount(4); //sent
    
  m_bc0Count[0] = optohybridDevice_->getBC0Count(0); //ttc
  m_bc0Count[1] = optohybridDevice_->getBC0Count(1); //internal/firmware
  m_bc0Count[2] = optohybridDevice_->getBC0Count(2); //external
  m_bc0Count[3] = optohybridDevice_->getBC0Count(3); //loopback 
  m_bc0Count[4] = optohybridDevice_->getBC0Count(4); //sent

  INFO("setTrigSource OH Trigger source 0x" << std::hex << confParams_.bag.triggerSource << std::dec);
  glibDevice_->flushFIFO(0);
  optohybridDevice_->sendResync();
  optohybridDevice_->sendBC0();
  optohybridDevice_->sendResync();
  optohybridDevice_->setTrigSource(confParams_.bag.triggerSource);

  hw_semaphore_.give();
  is_working_ = false;
  //start running
  wl_->submit(run_signature_);

  wl_->submit(select_signature_);
}

void gem::supervisor::GEMGLIBSupervisorWeb::stopAction(toolbox::Event::Reference evt) {
  is_running_ = false;
  //reset all counters?
  vfat_ = 0;
  event_ = 0;
  sumVFAT_ = 0;
  counter_ = {0,0,0};

  INFO("setTrigSource GLIB, OH mode 0");
  optohybridDevice_->setTrigSource(0x1);

  //turn off all chips?
  for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip) {
    (*chip)->setRunMode(0);
    INFO((*chip)->printErrorCounts());
  }
  wl_->submit(select_signature_);
}

void gem::supervisor::GEMGLIBSupervisorWeb::haltAction(toolbox::Event::Reference evt) {
  is_running_ = false;

  counter_ = {0,0,0};

  for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip) {
    (*chip)->setRunMode(0);
    INFO((*chip)->printErrorCounts());
  }
  wl_->submit(select_signature_);
  is_configured_ = false;
}

void gem::supervisor::GEMGLIBSupervisorWeb::noAction(toolbox::Event::Reference evt) {
}

void gem::supervisor::GEMGLIBSupervisorWeb::fireEvent(std::string name) {
  toolbox::Event::Reference event(new toolbox::Event(name, this));
  fsm_.fireEvent(event);
}

void gem::supervisor::GEMGLIBSupervisorWeb::stateChanged(toolbox::fsm::FiniteStateMachine &fsm) {
}

void gem::supervisor::GEMGLIBSupervisorWeb::transitionFailed(toolbox::Event::Reference event) {
}
