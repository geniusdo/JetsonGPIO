
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "JetsonGPIO.h"

#define MAX_EPOLL_EVENTS 20
#define READ_SIZE 10

namespace GPIO {
  void remove_edge_detect(int gpio);

  typedef struct __gpioEventObject {
    enum ModifyEvent { NONE, ADD, REMOVE, MODIFY } _epoll_change_flag;
    struct epoll_event _epoll_event;

    int channel_id;
    int gpio;
    int fd;
    Edge edge;
    uint64_t bounce_time;
    uint64_t previous_event;

    std::vector<void (*)(int)> callbacks;
  } _gpioEventObject;

  // typedef struct __gpioEventQueueRequest {
  //   enum RequestType {
  //     Add,
  //     AddCallback,
  //     Remove,
  //     RemoveCallback,
  //   };

  //   int gpio;
  //   RequestType request;

  //   union {
  //     struct {
  //       Edge edge;
  //       uint64_t bounce_time;
  //     };
  //     void (*callback)(int);
  //   };
  // } _gpioEventQueueRequest;

  std::recursive_mutex _epmutex;
  std::thread *_epoll_fd_thread = nullptr;
  std::atomic_bool _epoll_run_loop;

  std::map<int, std::shared_ptr<_gpioEventObject>> _gpio_events;
  std::atomic_int _auth_event_channel_count(0);
  std::map<int, int> _fd_to_gpio_map;

  // std::deque<_gpioEventQueueRequest> _gpio_event_queue;

  //----------------------------------

  int _write_sysfs_edge(int gpio, Edge edge, bool allow_none = true)
  {
    int result;
    char buf[256];
    snprintf(buf, 256, "%s/gpio%i/edge", _SYSFS_ROOT, gpio);
    int edge_fd = open(buf, O_WRONLY);
    if (edge_fd == -1) {
      // I/O Error
      fprintf(stderr, "Error opening file '%s'\n", buf);
      perror("_write_sysfs_edge");
      return -3;
    }
    switch (edge) {
      case Edge::RISING:
        result = write(edge_fd, "rising", 7);
        break;
      case Edge::FALLING:
        result = write(edge_fd, "falling", 7);
        break;
      case Edge::BOTH:
        result = write(edge_fd, "both", 7);
        break;
      case Edge::NONE: {
        if (!allow_none) {
          close(edge_fd);
          return -4;
        }
        result = write(edge_fd, "none", 7);
        break;
      }
      case Edge::UNKNOWN:
      default:
        fprintf(stderr, "Bad argument, edge=%i\n", (int)edge);
        close(edge_fd);
        return -5;
    }

    if (result == -1) {
      perror("Error writing value to sysfs/edge file");
    }
    else {
      result = 0;
    }

    close(edge_fd);
    return result;
  }

  int _open_sysfd_value(int gpio)
  {
    char buf[256];
    snprintf(buf, 256, "%s/gpio%i/value", _SYSFS_ROOT, gpio);
    int fd = open(buf, O_RDONLY);

    if (fd == -1) {
      // printf("[DEBUG] '%s' for given gpio '%i'\n", buf, gpio);
      return -1;
    }

    // Set the file descriptor to a non-blocking usage
    int result = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (result == -1) {
      perror("Failed to set the file descriptor to non-blocking");
    }

    return fd;
  }

  std::map<int, std::shared_ptr<_gpioEventObject>>::iterator
  _epoll_thread_remove_event(int epoll_fd, std::map<int, std::shared_ptr<_gpioEventObject>>::iterator geo_it)
  {
    auto geo = geo_it->second;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, geo->fd, 0) == -1) {
      fprintf(stderr, "Failed to delete file descriptor to epoll\n");
    }

    // Close the fd
    if (close(geo->fd) == -1) {
      fprintf(stderr, "Failed to close file descriptor\n");
    }

    // printf("[DEBUG] fd(%i:%i) removed from epoll\n", geo->channel_id, geo->gpio);

    // Erase from the map collection
    return _gpio_events.erase(geo_it);
  }

  void _epoll_thread_loop()
  {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
      fprintf(stderr, "Failed to create epoll file descriptor\n");
      return;
    }

    int e, event_count, result;
    epoll_event events[MAX_EPOLL_EVENTS];
    while (_epoll_run_loop) {
      // Wait a small time for events
      event_count = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 1);
      std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);

      // Handle Events
      if (event_count) {
        if (event_count < 0) {
          fprintf(stderr, "epoll_wait error\n");
          // TODO
          break;
        }

        // printf("[DEBUG] event_count=%i\n", event_count);

        // Handle event modifications
        auto tick = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

        // Iterate through each collected event
        for (e = 0; e < event_count; e++) {
          // Obtain the event object for the event
          auto gpio_it = _fd_to_gpio_map.find(events[e].data.fd);
          if (gpio_it == _fd_to_gpio_map.end()) {
            // puts("[DEBUG] Couldn't find gpio for fd-event");
            // Ignore it
            continue;
          }

          auto geo_it = _gpio_events.find(gpio_it->second);
          if (geo_it == _gpio_events.end()) {
            // TODO -- SHOULDN"T HAPPEN
            printf("[DEBUG] Couldn't find gpio object with gpio %i (OK if gpio edge was removed)\n", gpio_it->second);
            continue;
          }

          if (geo_it->second->_epoll_change_flag != _gpioEventObject::ModifyEvent::NONE) {
            // To be dealt with later. No events should be fired
            puts("[DEBUG] _epoll_change_flag was not NONE (OK if there was a ModifyEvent)");
            continue;
          }

          auto geo = geo_it->second;

          // Check event conditions
          if (geo->bounce_time) {
            if (tick - geo->previous_event < geo->bounce_time) {
              printf("[DEBUG] tick(%lu) - geo->previous_event(%lu), < geo->bounce_time(%lu)\n", tick,
                     geo->previous_event, geo->bounce_time);
              continue;
            }

            geo->previous_event = geo->bounce_time;
          }

          // Fire event callback(s)
          // printf("[DEBUG] event. callback-count:%zu\n", geo->callbacks.size());
          for (auto cb : geo->callbacks) {
            cb(geo->channel_id);
          }
        }
      }

      // Handle changes/modifications to GPIO event objects
      for (auto geo_it = _gpio_events.begin(); geo_it != _gpio_events.end();) {
        auto geo = geo_it->second;
        switch (geo->_epoll_change_flag) {
          case _gpioEventObject::ModifyEvent::NONE: {
            // No change

            // Iterate to next element
            geo_it++;
          } break;
          case _gpioEventObject::ModifyEvent::MODIFY: {
            // For now this just means modification of the edge type, which is done on the calling thread
            // Just set back to NONE and continue
            geo->_epoll_change_flag = _gpioEventObject::ModifyEvent::NONE;

            // Iterate to next element
            geo_it++;
          } break;
          case _gpioEventObject::ModifyEvent::ADD: {
            geo->_epoll_event.events = EPOLLIN | EPOLLPRI | EPOLLET;
            geo->_epoll_event.data.fd = geo->fd;

            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, geo->fd, &geo->_epoll_event) == -1) {
              fprintf(stderr, "Failed to add file descriptor to epoll (gpio=%i)\n", geo->gpio);
              // TODO
            }

            geo->_epoll_change_flag = _gpioEventObject::ModifyEvent::NONE;
            puts("[DEBUG] fd added to epoll");

            // Iterate to next element
            geo_it++;
          } break;
          case _gpioEventObject::ModifyEvent::REMOVE: {
            printf("Ack remove command from %i:%i\n", geo_it->first, geo_it->second->channel_id);
            geo_it = _epoll_thread_remove_event(epoll_fd, geo_it);

            // Break and do not iterate past the returned element
          } break;
          default: {
            // Iterate to next element
            geo_it++;
          } break;
        }
      }
    }

    puts("[DEBUG] EPOLL thread closing: GPIO-cleanup");
    // Thread is coming to an end
    // -- Cleanup
    // GPIO Event Objects
    {
      std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);
      for (auto geo_it = _gpio_events.begin(); geo_it != _gpio_events.end();) {
        geo_it = _epoll_thread_remove_event(epoll_fd, geo_it);
      }
    }

    puts("[DEBUG] EPOLL thread closing: epoll_fd");

    // epoll
    if (close(epoll_fd) == -1) {
      fprintf(stderr, "Failed to close epoll file descriptor\n");
    }

    // DEBUG
    puts("EPOLL thread exited!");
    // DEBUG

    //   printf("%d ready events\n", event_count);
    //   for (i = 0; i < event_count; i++) {
    //     if (events[i].data.fd == gfd) {
    //       // printf("here\n");
    //       printf("Reading file descriptor '%d' (%u) -- \n", events[i].data.fd, events[i].data.u32);
    //       bytes_read = read(events[i].data.fd, read_buffer, READ_SIZE);
    //       if (bytes_read == -1) {
    //         if (EAGAIN == errno || EWOULDBLOCK == errno) {
    //           printf("EAGAIN\n");
    //           continue;
    //         }

    //         if (EINTR == errno) {
    //           printf("EINTR\n");
    //           break;
    //         }

    //         perror("read() failed");

    //         running = event_count = 0;
    //         break;
    //       }
    //       else {
    //         printf("%zd bytes read.\n", bytes_read);
    //         read_buffer[bytes_read] = '\0';
    //         printf("Read '%s'\n", read_buffer);
    //       }
    //     }
    //   }
    // }
  }

  void _epoll_start_thread()
  {
    _epoll_run_loop = true;
    _epoll_fd_thread = new std::thread(_epoll_thread_loop);
  }

  void _event_cleanup(int gpio)
  {
    // // DEBUG
    // bool removed = false;
    // printf("[DEBUG]_event_cleanup(%i)\n", gpio);
    // // DEBUG

    remove_edge_detect(gpio);
    // {
    //   // Enter Mutex
    //   std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);
    //   if (_gpio_events.find(gpio) != _gpio_events.end()) {
    //     // DEBUG
    //     removed = true;
    //     // DEBUG
    //     puts("ec-lock found");
    //   }
    // }

    // // DEBUG
    // int t = _auth_event_channel_count;
    // printf("[DEBUG]exited _event_cleanup(%i): %s, %i gpio-objects left\n", gpio,
    //        removed ? " channel event removed" : " no event found for channel", t);
    // // DEBUG
  }

  //----------------------------------

  void blocking_wait_for_edge() {}

  int add_edge_detect(int gpio, int channel_id, Edge edge, uint64_t bounce_time)
  {
    int result;

    // Enter Mutex
    std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);

    // auto geo;
    std::shared_ptr<_gpioEventObject> geo;
    auto find_result = _gpio_events.find(gpio);
    if (find_result != _gpio_events.end()) {
      geo = find_result->second;

      if (geo->_epoll_change_flag == _gpioEventObject::ModifyEvent::REMOVE) {
        // The epoll thread is inbetween concurrent transactions. Modify the existing
        // object instead of removing it

        if (geo->edge != edge) {
          // Update
          geo->_epoll_change_flag = _gpioEventObject::ModifyEvent::MODIFY;
          geo->edge = edge;
          // Set Event
          result = _write_sysfs_edge(gpio, edge);
          if (result) {
            fprintf(stderr, "Failed to write edge value to sys-fs (1)\n");
            return result;
          }
        }
        geo->bounce_time = bounce_time;
        geo->previous_event = 0;
        ++_auth_event_channel_count;
      }
      else if (geo->edge != edge) {
        fprintf(stderr, "Cannot have conflicting event types for a single gpio\n");
        return -1;
      }
      else if (!bounce_time && geo->bounce_time != bounce_time) {
        fprintf(stderr, "Cannot have multiple conflicting bounce_times for a single gpio\n");
        return -2;
      }
    }
    else {
      // Configure anew
      geo = std::make_shared<_gpioEventObject>();
      geo->_epoll_change_flag = _gpioEventObject::ModifyEvent::ADD;
      geo->gpio = gpio;
      geo->channel_id = channel_id;
      geo->edge = edge;
      geo->bounce_time = bounce_time;
      geo->previous_event = 0;

      // Open the value fd
      geo->fd = _open_sysfd_value(gpio);
      if (geo->fd == -1) {
        perror("Failed to open sys-file descriptor for gpio value");
        return -3;
      }
      _fd_to_gpio_map[geo->fd] = gpio;

      // Set Event
      result = _write_sysfs_edge(gpio, edge);
      if (result) {
        fprintf(stderr, "Failed to write edge value to sys-fs (2)\n");
        close(geo->fd);
        return result;
      }

      // Set
      _gpio_events[gpio] = geo;
      ++_auth_event_channel_count;

      if (!_epoll_fd_thread) {
        _epoll_start_thread();
      }
    }

    return 0;
  }

  void remove_edge_detect(int gpio)
  {
    // printf("[DEBUG]remove_edge_detect(%i)\n", gpio);

    // Enter Mutex
    std::unique_lock<std::recursive_mutex> mutex_lock(_epmutex);

    auto find_result = _gpio_events.find(gpio);
    if (find_result != _gpio_events.end()) {
      auto geo = find_result->second;
      geo->_epoll_change_flag = _gpioEventObject::ModifyEvent::REMOVE;

      auto fg_it = _fd_to_gpio_map.find(geo->fd);
      if (fg_it != _fd_to_gpio_map.end())
        _fd_to_gpio_map.erase(fg_it);

      --_auth_event_channel_count;
      if (_auth_event_channel_count == 0 && _epoll_fd_thread) {
        // // DEBUG
        // printf("[DEBUG]_epoll_fd_thread=%p shutting down\n", _epoll_fd_thread);
        // // DEBUG

        // Signal shutdown of thread
        _epoll_run_loop = false;
        mutex_lock.unlock();

        // Wait to join
        _epoll_fd_thread->join();

        // Resume lock and clear thread
        mutex_lock.lock();
        delete _epoll_fd_thread;
        _epoll_fd_thread = nullptr;
      }

      // // DEBUG
      // printf("[DEBUG]remove_edge_detect(%i) REMOVED-ITEM\n", gpio);
      // // DEBUG
    }
    // // DEBUG
    // else
    //   printf("[DEBUG]remove_edge_detect(%i) REDUNDANT-CALL\n", gpio);
    // // DEBUG
  }

  void add_edge_callback(int gpio, void (*callback)(int))
  {
    std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);

    auto find_result = _gpio_events.find(gpio);
    if (find_result == _gpio_events.end()) {
      // TODO Exception
      printf("ERROR TODO 1\n");
      return;
    }

    auto geo = find_result->second;
    geo->callbacks.push_back(callback);
  }

  void remove_edge_callback(int gpio, void (*callback)(int))
  {
    std::lock_guard<std::recursive_mutex> mutex_lock(_epmutex);

    auto find_result = _gpio_events.find(gpio);
    if (find_result == _gpio_events.end()) {
      // TODO Exception
      printf("ERROR TODO 2\n");
      return;
    }

    auto geo = find_result->second;
    for (auto geo_it = geo->callbacks.begin(); geo_it != geo->callbacks.end();) {
      if (*geo_it == callback) {
        geo->callbacks.erase(geo_it);
      }
      else {
        geo_it++;
      }
    }
  }

} // namespace GPIO