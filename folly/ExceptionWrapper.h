/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOLLY_EXCEPTIONWRAPPER_H
#define FOLLY_EXCEPTIONWRAPPER_H

#include <cassert>
#include <exception>
#include <memory>
#include <folly/String.h>
#include <folly/detail/ExceptionWrapper.h>

namespace folly {

/*
 * Throwing exceptions can be a convenient way to handle errors. Storing
 * exceptions in an exception_ptr makes it easy to handle exceptions in a
 * different thread or at a later time. exception_ptr can also be used in a very
 * generic result/exception wrapper.
 *
 * However, there are some issues with throwing exceptions and
 * std::exception_ptr. These issues revolve around throw being expensive,
 * particularly in a multithreaded environment (see
 * ExceptionWrapperBenchmark.cpp).
 *
 * Imagine we have a library that has an API which returns a result/exception
 * wrapper. Let's consider some approaches for implementing this wrapper.
 * First, we could store a std::exception. This approach loses the derived
 * exception type, which can make exception handling more difficult for users
 * that prefer rethrowing the exception. We could use a folly::dynamic for every
 * possible type of exception. This is not very flexible - adding new types of
 * exceptions requires a change to the result/exception wrapper. We could use an
 * exception_ptr. However, constructing an exception_ptr as well as accessing
 * the error requires a call to throw. That means that there will be two calls
 * to throw in order to process the exception. For performance sensitive
 * applications, this may be unacceptable.
 *
 * exception_wrapper is designed to handle exception management for both
 * convenience and high performance use cases. make_exception_wrapper is
 * templated on derived type, allowing us to rethrow the exception properly for
 * users that prefer convenience. These explicitly named exception types can
 * therefore be handled without any peformance penalty.  exception_wrapper is
 * also flexible enough to accept any type. If a caught exception is not of an
 * explicitly named type, then std::exception_ptr is used to preserve the
 * exception state. For performance sensitive applications, the accessor methods
 * can test or extract a pointer to a specific exception type with very little
 * overhead.
 *
 * Example usage:
 *
 * exception_wrapper globalExceptionWrapper;
 *
 * // Thread1
 * void doSomethingCrazy() {
 *   int rc = doSomethingCrazyWithLameReturnCodes();
 *   if (rc == NAILED_IT) {
 *     globalExceptionWrapper = exception_wrapper();
 *   } else if (rc == FACE_PLANT) {
 *     globalExceptionWrapper = make_exception_wrapper<FacePlantException>();
 *   } else if (rc == FAIL_WHALE) {
 *     globalExceptionWrapper = make_exception_wrapper<FailWhaleException>();
 *   }
 * }
 *
 * // Thread2: Exceptions are ok!
 * void processResult() {
 *   try {
 *     globalExceptionWrapper.throwException();
 *   } catch (const FacePlantException& e) {
 *     LOG(ERROR) << "FACEPLANT!";
 *   } catch (const FailWhaleException& e) {
 *     LOG(ERROR) << "FAILWHALE!";
 *   }
 * }
 *
 * // Thread2: Exceptions are bad!
 * void processResult() {
 *   auto ep = globalExceptionWrapper.get();
 *   if (!ep.with_exception<FacePlantException>([&](
 *     FacePlantException& faceplant) {
 *       LOG(ERROR) << "FACEPLANT";
 *     })) {
 *     ep.with_exception<FailWhaleException>([&](
 *       FailWhaleException& failwhale) {
 *         LOG(ERROR) << "FAILWHALE!";
 *       });
 *   }
 * }
 *
 */
class exception_wrapper {
 public:
  exception_wrapper() : throwfn_(nullptr) { }

  void throwException() const {
    if (throwfn_) {
      throwfn_(item_.get());
    } else if (eptr_) {
      std::rethrow_exception(eptr_);
    }
  }

  explicit operator bool() const {
    return item_ || eptr_;
  }

  // This implementation is similar to std::exception_ptr's implementation
  // where two exception_wrappers are equal when the address in the underlying
  // reference field both point to the same exception object.  The reference
  // field remains the same when the exception_wrapper is copied or when
  // the exception_wrapper is "rethrown".
  bool operator==(const exception_wrapper& a) const {
    if (item_) {
      return a.item_ && item_.get() == a.item_.get();
    } else {
      return eptr_ == a.eptr_;
    }
  }

  bool operator!=(const exception_wrapper& a) const {
    return !(*this == a);
  }

  // This will return a non-nullptr only if the exception is held as a
  // copy.  It is the only interface which will distinguish between an
  // exception held this way, and by exception_ptr.  You probably
  // shouldn't use it at all.
  std::exception* getCopied() { return item_.get(); }
  const std::exception* getCopied() const { return item_.get(); }

  fbstring what() const {
    if (item_) {
      return exceptionStr(*item_.get());
    } else if (eptr_) {
      return estr_;
    } else {
      return fbstring();
    }
  }

  template <class Ex>
  bool is_compatible_with() const {
    if (item_) {
      return dynamic_cast<const Ex*>(getCopied());
    } else if (eptr_) {
      try {
        std::rethrow_exception(eptr_);
      } catch (std::exception& e) {
        return dynamic_cast<const Ex*>(&e);
      } catch (...) {
        // fall through
      }
    }
    return false;
  }

  template <class Ex, class F>
  bool with_exception(F f) {
    if (item_) {
      if (auto ex = dynamic_cast<Ex*>(getCopied())) {
        f(*ex);
        return true;
      }
    } else if (eptr_) {
      try {
        std::rethrow_exception(eptr_);
      } catch (std::exception& e) {
        if (auto ex = dynamic_cast<Ex*>(&e)) {
          f(*ex);
          return true;
        }
      } catch (...) {
        // fall through
      }
    }
    return false;
  }

  template <class Ex, class F>
  bool with_exception(F f) const {
    return with_exception<const Ex>(f);
  }

  std::exception_ptr getExceptionPtr() const {
    if (eptr_) {
      return eptr_;
    }

    try {
      throwException();
    } catch (...) {
      return std::current_exception();
    }
    return std::exception_ptr();
  }

 protected:
  // Optimized case: if we know what type the exception is, we can
  // store a copy of the concrete type, and a helper function so we
  // can rethrow it.
  std::shared_ptr<std::exception> item_;
  void (*throwfn_)(std::exception*);
  // Fallback case: store the library wrapper, which is less efficient
  // but gets the job done.  Also store the the what() string, so we
  // can at least get it back out without having to rethrow.
  std::exception_ptr eptr_;
  std::string estr_;

  template <class T, class... Args>
  friend exception_wrapper make_exception_wrapper(Args&&... args);
};

template <class T, class... Args>
exception_wrapper make_exception_wrapper(Args&&... args) {
  exception_wrapper ew;
  ew.item_ = std::make_shared<T>(std::forward<Args>(args)...);
  ew.throwfn_ = folly::detail::Thrower<T>::doThrow;
  return ew;
}

/*
 * try_and_catch is a simple replacement for try {} catch(){} that allows you to
 * specify which derived exceptions you would like to catch and store in an
 * exception_wrapper.
 *
 * Because we cannot build an equivalent of std::current_exception(), we need
 * to catch every derived exception that we are interested in catching.
 *
 * Exceptions should be listed in the reverse order that you would write your
 * catch statements (that is, std::exception& should be first).
 *
 * NOTE: Although implemented as a derived class (for syntactic delight), don't
 * be confused - you should not pass around try_and_catch objects!
 *
 * Example Usage:
 *
 * // This catches my runtime_error and if I call throwException() on ew, it
 * // will throw a runtime_error
 * auto ew = folly::try_and_catch<std::exception, std::runtime_error>([=]() {
 *   if (badThingHappens()) {
 *     throw std::runtime_error("ZOMG!");
 *   }
 * });
 *
 * // This will catch the exception and if I call throwException() on ew, it
 * // will throw a std::exception
 * auto ew = folly::try_and_catch<std::exception, std::runtime_error>([=]() {
 *   if (badThingHappens()) {
 *     throw std::exception();
 *   }
 * });
 *
 * // This will not catch the exception and it will be thrown.
 * auto ew = folly::try_and_catch<std::runtime_error>([=]() {
 *   if (badThingHappens()) {
 *     throw std::exception();
 *   }
 * });
 */

template <typename... Exceptions>
class try_and_catch;

template <typename LastException, typename... Exceptions>
class try_and_catch<LastException, Exceptions...> :
    public try_and_catch<Exceptions...> {
 public:
  template <typename F>
  explicit try_and_catch(F&& fn) : Base() {
    call_fn(fn);
  }

 protected:
  typedef try_and_catch<Exceptions...> Base;

  try_and_catch() : Base() {}

  template <typename Ex>
  typename std::enable_if<std::is_base_of<std::exception, Ex>::value>::type
  assign_eptr(Ex& e) {
    this->eptr_ = std::current_exception();
    this->estr_ = exceptionStr(e).toStdString();
  }

  template <typename Ex>
  typename std::enable_if<!std::is_base_of<std::exception, Ex>::value>::type
  assign_eptr(Ex& e) {
    this->eptr_ = std::current_exception();
    this->estr_ = exceptionStr(e).toStdString();
  }

  template <typename Ex>
  struct optimize {
    static const bool value =
      std::is_base_of<std::exception, Ex>::value &&
      std::is_copy_assignable<Ex>::value &&
      !std::is_abstract<Ex>::value;
  };

  template <typename Ex>
  typename std::enable_if<!optimize<Ex>::value>::type
  assign_exception(Ex& e) {
    assign_eptr(e);
  }

  template <typename Ex>
  typename std::enable_if<optimize<Ex>::value>::type
  assign_exception(Ex& e) {
    this->item_ = std::make_shared<Ex>(e);
    this->throwfn_ = folly::detail::Thrower<Ex>::doThrow;
  }

  template <typename F>
  void call_fn(F&& fn) {
    try {
      Base::call_fn(std::move(fn));
    } catch (LastException& e) {
      if (typeid(e) == typeid(LastException&)) {
        assign_exception(e);
      } else {
        assign_eptr(e);
      }
    }
  }
};

template<>
class try_and_catch<> : public exception_wrapper {
 public:
  try_and_catch() {}

 protected:
  template <typename F>
  void call_fn(F&& fn) {
    fn();
  }
};
}
#endif
