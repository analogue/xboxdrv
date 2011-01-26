/*
**  Xbox360 USB Gamepad Userspace Driver
**  Copyright (C) 2011 Ingo Ruhnke <grumbel@gmx.de>
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "xboxdrv_daemon.hpp"

#include <boost/format.hpp>
#include <fstream>
#include <stdexcept>

#include "default_message_processor.hpp"
#include "dummy_message_processor.hpp"
#include "helper.hpp"
#include "log.hpp"
#include "raise_exception.hpp"
#include "uinput.hpp"
#include "usb_helper.hpp"
#include "xbox_controller_factory.hpp"
#include "xboxdrv_thread.hpp"

extern bool global_exit_xboxdrv;

namespace {

bool get_usb_id(udev_device* device, uint16_t* vendor_id, uint16_t* product_id)
{
  const char* vendor_id_str  = udev_device_get_property_value(device, "ID_VENDOR_ID");
  if (!vendor_id_str)
  {
    return false;
  }
  else
  {
    *vendor_id = hexstr2int(vendor_id_str);
  }

  const char* product_id_str = udev_device_get_property_value(device, "ID_MODEL_ID");
  if (!product_id_str)
  {
    return false;
  }
  else
  {
    *product_id = hexstr2int(product_id_str);
  }

  return true;
}

bool get_usb_path(udev_device* device, int* bus, int* dev)
{
  // busnum:devnum are decimal, not hex
  const char* busnum_str = udev_device_get_property_value(device, "BUSNUM");
  if (!busnum_str)
  {
    return false;
  }
  else
  {
    *bus = boost::lexical_cast<int>(busnum_str);
  }

  const char* devnum_str = udev_device_get_property_value(device, "DEVNUM");
  if (!devnum_str)
  {
    return false;
  }
  else
  {
    *dev = boost::lexical_cast<int>(devnum_str);
  }

  return true;
}

} // namespace

XboxdrvDaemon::XboxdrvDaemon() :
  m_udev(0),
  m_monitor(0),
  m_controller_slots(),
  m_uinput()
{
}

XboxdrvDaemon::~XboxdrvDaemon()
{
  for(ControllerSlots::iterator i = m_controller_slots.begin(); i != m_controller_slots.end(); ++i)
  {
    delete i->thread;
    i->thread = 0;
  }

  udev_monitor_unref(m_monitor);
  udev_unref(m_udev);
}

void
XboxdrvDaemon::cleanup_threads()
{
  int count = 0;

  for(ControllerSlots::iterator i = m_controller_slots.begin(); i != m_controller_slots.end(); ++i)
  {
    if (i->thread)
    {
      if (i->thread->try_join_thread())
      {
        delete i->thread;
        i->thread = 0;
        count += 1;
      }
    }
  }

  if (count > 0)
  {
    log_info << "cleaned up " << count << " thread(s), free slots: " 
             << get_free_slot_count() << "/" << m_controller_slots.size()
             << std::endl;
  }
}

void
XboxdrvDaemon::process_match(const Options& opts, struct udev_device* device)
{
  if (false)
  {
    print_info(device);
  }

  uint16_t vendor;
  uint16_t product;

  if (!get_usb_id(device, &vendor, &product))
  {
    log_warning << "couldn't get vendor:product" << std::endl;
  }
  else
  {
    XPadDevice dev_type;
    if (!find_xpad_device(vendor, product, &dev_type))
    {
      log_info << "ignoring " << boost::format("%04x:%04x") % vendor % product
               << " not a valid Xboxdrv device" << std::endl;
    }
    else
    {  
      int bus;
      int dev;
      if (!get_usb_path(device, &bus, &dev))
      {
        log_warning << "couldn't get bus:dev" << std::endl;
      }
      else
      {
        ControllerSlot* slot = find_free_slot(vendor, product, bus, dev);
        if (!slot)
        {
          log_error << "no free controller slot found, controller will be ignored" << std::endl;
        }
        else
        {
          try 
          {
            launch_xboxdrv(dev_type, opts, bus, dev, *slot);
          }
          catch(const std::exception& err)
          {
            log_error << "failed to launch XboxdrvThread: " << err.what() << std::endl;
          }
        }
      }
    }
  }
}

void
XboxdrvDaemon::init_uinput(const Options& opts)
{
  // Setup uinput
  if (opts.no_uinput)
  {
    log_info << "starting without UInput" << std::endl;

    // just create some empty controller slots
    m_controller_slots.resize(opts.controller_slots.size());
  }
  else
  {
    log_info << "starting with UInput" << std::endl;

    m_uinput.reset(new UInput());

    // FIXME:
    /* must setup this callback later when we have a controller
       if (opts.uinput_config.force_feedback)
       {
       uinput->set_ff_callback(boost::bind(&set_rumble,  controller.get(), opts.rumble_gain, _1, _2));
       }
    */

    // create controller slots
    int slot_count = 0;

    for(Options::ControllerSlots::const_iterator controller = opts.controller_slots.begin(); 
        controller != opts.controller_slots.end(); ++controller)
    {
      log_info << "creating slot: " << slot_count << std::endl;
      m_controller_slots.push_back(ControllerSlot(m_controller_slots.size(),
                                                  ControllerConfigSet::create(*m_uinput, slot_count,
                                                                              opts.extra_devices,
                                                                              controller->second),
                                                  controller->second.get_match_rules()));
      slot_count += 1;
    }

    log_info << "created " << m_controller_slots.size() << " controller slots" << std::endl;

    // After all the ControllerConfig registered their events, finish up
    // the device creation
    m_uinput->finish();
  }
}

void
XboxdrvDaemon::init_udev()
{
  assert(!m_udev);

  m_udev = udev_new();
   
  if (!m_udev)
  {
    raise_exception(std::runtime_error, "udev init failure");
  }
}

void
XboxdrvDaemon::init_udev_monitor(const Options& opts)
{
  // Setup udev monitor and enumerate
  m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
  udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "usb", "usb_device");
  udev_monitor_enable_receiving(m_monitor);

  // FIXME: won't we get devices twice that have been plugged in at
  // this point? once from the enumeration, once from the monitor

  // Enumerate over all devices already connected to the computer
  {
    struct udev_enumerate* enumerate = udev_enumerate_new(m_udev);
    assert(enumerate);

    udev_enumerate_add_match_subsystem(enumerate, "usb");
    // not available yet: udev_enumerate_add_match_is_initialized(enumerate);
    udev_enumerate_scan_devices(enumerate);

    struct udev_list_entry* devices;
    struct udev_list_entry* dev_list_entry;
    
    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(dev_list_entry, devices) 
    {
      // name is path, value is NULL
      const char* path = udev_list_entry_get_name(dev_list_entry);

      struct udev_device* device = udev_device_new_from_syspath(m_udev, path);

      // manually filter for devtype, as udev enumerate can't do it by itself
      const char* devtype = udev_device_get_devtype(device);
      if (devtype && strcmp(devtype, "usb_device") == 0)
      {
        process_match(opts, device);
      }
      udev_device_unref(device);
    }
    udev_enumerate_unref(enumerate);
  } 
}

void
XboxdrvDaemon::create_pid_file(const Options& opts)
{
  if (!opts.pid_file.empty())
  {
    log_info << "writing pid file: " << opts.pid_file << std::endl;
    std::ofstream out(opts.pid_file.c_str());
    if (!out)
    {
      raise_exception(std::runtime_error, "failed to create pid file: " << opts.pid_file << ": " << strerror(errno));
    }
    else
    {
      out << getpid() << std::endl;
    }
  }
}

void
XboxdrvDaemon::run(const Options& opts)
{
  try 
  {
    create_pid_file(opts);

    init_uinput(opts);
    init_udev();
    init_udev_monitor(opts);

    run_loop(opts);
  }
  catch(const std::exception& err)
  {
    log_error << "fatal exception: " << err.what() << std::endl;
  }
}

void
XboxdrvDaemon::run_loop(const Options& opts)
{
  while(!global_exit_xboxdrv)
  {
    // FIXME: udev_monitor_receive_device() will block, must break out of it somehow
    // FIXME: we bust udev_unref_monitor() this
    struct udev_device* device = udev_monitor_receive_device(m_monitor);

    cleanup_threads();

    if (!device)
    {
      // seem to be normal, do we get this when the given device is filtered out?
      std::cout << "udev device couldn't be read: " << device << std::endl;
    }
    else
    {
      const char* action = udev_device_get_action(device);

      if (action && strcmp(action, "add") == 0)
      {
        process_match(opts, device);
      }
    }
    udev_device_unref(device);
  }
}

void
XboxdrvDaemon::print_info(struct udev_device* device)
{
  std::cout << "/---------------------------------------------" << std::endl;
  std::cout << "devpath: " << udev_device_get_devpath(device) << std::endl;
  
  if (udev_device_get_action(device))
    std::cout << "action: " << udev_device_get_action(device) << std::endl;
  //std::cout << "init: " << udev_device_get_is_initialized(device) << std::endl;

  if (udev_device_get_subsystem(device))
    std::cout << "subsystem: " << udev_device_get_subsystem(device) << std::endl;

  if (udev_device_get_devtype(device))
    std::cout << "devtype:   " << udev_device_get_devtype(device) << std::endl;

  if (udev_device_get_syspath(device))
    std::cout << "syspath:   " << udev_device_get_syspath(device) << std::endl;

  if (udev_device_get_sysname(device))
    std::cout << "sysname:   " << udev_device_get_sysname(device) << std::endl;

  if (udev_device_get_sysnum(device))
    std::cout << "sysnum:    " << udev_device_get_sysnum(device) << std::endl;

  if (udev_device_get_devnode(device))
    std::cout << "devnode:   " << udev_device_get_devnode(device) << std::endl;

  if (udev_device_get_driver(device))
    std::cout << "driver:    " << udev_device_get_driver(device) << std::endl;

  if (udev_device_get_action(device))
    std::cout << "action:    " << udev_device_get_action(device) << std::endl;
          
  //udev_device_get_sysattr_value(device, "busnum");
  //udev_device_get_sysattr_value(device, "devnum");

  {
    std::cout << "list: " << std::endl;
    struct udev_list_entry* it = udev_device_get_tags_list_entry(device);
    while((it = udev_list_entry_get_next(it)) != 0)
    {         
      std::cout << "  " 
                << udev_list_entry_get_name(it) << " = "
                << udev_list_entry_get_value(it)
                << std::endl;
    }
  }
          
  {
    std::cout << "properties: " << std::endl;
    struct udev_list_entry* it = udev_device_get_properties_list_entry(device);
    while((it = udev_list_entry_get_next(it)) != 0)
    {         
      std::cout << "  " 
                << udev_list_entry_get_name(it) << " = "
                << udev_list_entry_get_value(it)
                << std::endl;
    }
  }
          
  {
    std::cout << "devlist: " << std::endl;
    struct udev_list_entry* it = udev_device_get_tags_list_entry(device);
    while((it = udev_list_entry_get_next(it)) != 0)
    {         
      std::cout << "  " 
                << udev_list_entry_get_name(it) << " = "
                << udev_list_entry_get_value(it)
                << std::endl;
    }
  }

  std::cout << "\\----------------------------------------------" << std::endl;
}

XboxdrvDaemon::ControllerSlot*
XboxdrvDaemon::find_free_slot(uint16_t vendor, uint16_t product,
                              int bus, int dev) const
{
  // first pass, look for slots where the rules match the given vendor:product, bus:dev
  for(ControllerSlots::const_iterator i = m_controller_slots.begin(); i != m_controller_slots.end(); ++i)
  {
    if (i->thread == 0)
    {
      // found a free slot, check if the rules match
      for(std::vector<ControllerMatchRule>::const_iterator rule = i->rules.begin(); rule != i->rules.end(); ++rule)
      {
        if (rule->match(vendor, product, bus, dev))
        {
          // FIXME: ugly const_cast
          return const_cast<ControllerSlot*>(&(*i));
        }
      }
    }
  }

  // second path, look for slots that don't have any rules and thus match everything
  for(ControllerSlots::const_iterator i = m_controller_slots.begin(); i != m_controller_slots.end(); ++i)
  {
    if (i->thread == 0 && i->rules.empty())
    {
      // FIXME: ugly const_cast
      return const_cast<ControllerSlot*>(&(*i));
    }
  }
    
  // no free slot found
  return 0;
}

void
XboxdrvDaemon::launch_xboxdrv(const XPadDevice& dev_type, const Options& opts, 
                              uint8_t busnum, uint8_t devnum,
                              ControllerSlot& slot)
{
  // FIXME: results must be libusb_unref_device()'ed
  libusb_device* dev = usb_find_device_by_path(busnum, devnum);

  if (!dev)
  {
    log_error << "USB device disappeared before it could be opened" << std::endl;
  }
  else
  {
    std::auto_ptr<XboxGenericController> controller = XboxControllerFactory::create(dev_type, dev, opts);

    std::auto_ptr<MessageProcessor> message_proc;
    if (m_uinput.get())
    {
      message_proc.reset(new DefaultMessageProcessor(*m_uinput, slot.config, opts));
    }
    else
    {
      message_proc.reset(new DummyMessageProcessor());
    }

    std::auto_ptr<XboxdrvThread> thread(new XboxdrvThread(message_proc, controller, opts));
    thread->start_thread(opts);
    slot.thread = thread.release();

    log_info << "launched XboxdrvThread for " << boost::format("%03d:%03d")
      % static_cast<int>(busnum) 
      % static_cast<int>(devnum)
             << " in slot " << slot.id << ", free slots: " 
             << get_free_slot_count() << "/" << m_controller_slots.size()
             << std::endl;
  }
}

int
XboxdrvDaemon::get_free_slot_count() const
{
  int slot_count = 0;

  for(ControllerSlots::const_iterator i = m_controller_slots.begin(); i != m_controller_slots.end(); ++i)
  {
    if (i->thread == 0)
    {
      slot_count += 1;
    }
  }

  return slot_count;
}

/* EOF */
