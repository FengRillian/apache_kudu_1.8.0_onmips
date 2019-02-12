// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#ifndef KUDU_UTIL_CURL_UTIL_H
#define KUDU_UTIL_CURL_UTIL_H

#include <string>
#include <vector>

#include "kudu/gutil/macros.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

typedef void CURL;

namespace kudu {

class faststring;

// Simple wrapper around curl's "easy" interface, allowing the user to
// fetch web pages into memory using a blocking API.
//
// This is not thread-safe.
class EasyCurl {
 public:
  EasyCurl();
  ~EasyCurl();

  // Fetch the given URL into the provided buffer.
  // Any existing data in the buffer is replaced.
  // The optional param 'headers' holds additional headers.
  // e.g. {"Accept-Encoding: gzip"}
  Status FetchURL(const std::string& url,
                  faststring* dst,
                  const std::vector<std::string>& headers = {});

  // Issue an HTTP POST to the given URL with the given data.
  // Returns results in 'dst' as above.
  Status PostToURL(const std::string& url,
                   const std::string& post_data,
                   faststring* dst);

  // Set whether to verify the server's SSL certificate in the case of an HTTPS
  // connection.
  void set_verify_peer(bool verify) {
    verify_peer_ = verify;
  }

  void set_return_headers(bool v) {
    return_headers_ = v;
  }

  void set_timeout(MonoDelta t) {
    timeout_ = t;
  }

 private:
  // Do a request. If 'post_data' is non-NULL, does a POST.
  // Otherwise, does a GET.
  Status DoRequest(const std::string& url,
                   const std::string* post_data,
                   faststring* dst,
                   const std::vector<std::string>& headers = {});
  CURL* curl_;

  // Whether to verify the server certificate.
  bool verify_peer_ = true;

  // Whether to return the HTTP headers with the response.
  bool return_headers_ = false;

  MonoDelta timeout_;

  DISALLOW_COPY_AND_ASSIGN(EasyCurl);
};

} // namespace kudu

#endif /* KUDU_UTIL_CURL_UTIL_H */
