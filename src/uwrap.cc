
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

#include <v8.h>
#include <node.h>
#include <nan.h>

namespace uwrap {

	using std::move;
	using std::string;
	using std::vector;
	using std::function;

	#define _STR(a) #a
	#define __STR(a) _STR(a)
	#define PATH_LINE() __FILE__ ":" __STR(__LINE__)


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	template <typename Value>
	struct ThreadWork {
		ThreadWork() { handle.data = nullptr; }

		void operator () (function<Value()> work, function<void(Value)> after) {
			assert(idle());
			handle.data = this;
			this->work = move(work);
			this->after = move(after);
			uv_queue_work(uv_default_loop(), &handle, ThreadWork<Value>::work_thunk, ThreadWork<Value>::after_thunk);
		}

		static void work_thunk(uv_work_t *req) {
			ThreadWork<Value> *self = static_cast<ThreadWork<Value>*>(req->data);
			assert(self != nullptr);
			self->value = self->work();
		}

		static void after_thunk(uv_work_t *req, int status) {
			ThreadWork<Value> *self = static_cast<ThreadWork<Value>*>(req->data);
			assert(self != nullptr);
			function<void(Value)> after = move(self->after);
			self->handle.data = nullptr;
			after(move(self->value));
		}

		bool idle() { return handle.data == nullptr; }

		uv_work_t handle;
		function<Value()> work;
		function<void(Value)> after;
		Value value;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	struct ErrorResult {
		ErrorResult() { code = 0; }
		ErrorResult(string m) { message = move(m); code = -1; }
		ErrorResult(int c, string sc, string m, string p) {
			code = c == 0 ? -1 : c;
			syscall = move(sc);
			message = move(m);
			path = move(p);
		}

		bool isError() { return code != 0; }

		v8::Handle<v8::Value> makeError() {
			if (syscall.empty() || code < 0)
				return Nan::Error(message.c_str());
			return Nan::ErrnoException(code, syscall.c_str(), message.c_str(), path.c_str());
		}

		string syscall;
		string message;
		string path;
		int code;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	struct SocketResult : public ErrorResult {
		using ErrorResult::ErrorResult;

		SocketResult() { descriptor = 0; }
		SocketResult(int fd) { descriptor = fd; }

		int descriptor;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	struct BoolResult : public ErrorResult {
		using ErrorResult::ErrorResult;

		BoolResult(bool v) { ok = v; }

		bool ok;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	template <typename SubClass>
	struct UWrap : public Nan::ObjectWrap {

		static void init(v8::Handle<v8::Object> target) {
			v8::Local<v8::String> className = Nan::New(SubClass::className()).ToLocalChecked();
			v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
			tpl->InstanceTemplate()->SetInternalFieldCount(1);
			tpl->SetClassName(className);

			SubClass::v8Init(tpl);

			SetPrototypeMethod(tpl, "pause", pause);
			SetPrototypeMethod(tpl, "resume", resume);

			SetPrototypeMethod(tpl, "close", close);

			constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
			Nan::Set(target, className, Nan::GetFunction(tpl).ToLocalChecked());
		}

		static inline Nan::Persistent<v8::Function> &constructor() {
			static Nan::Persistent<v8::Function> my_constructor;
			return my_constructor;
		}

		static NAN_METHOD(New) {
			if (info.IsConstructCall()) {
				SubClass *obj = new SubClass();
				obj->Wrap(info.This());
				if (!info[0]->IsFunction()) {
					Nan::ThrowError((string(SubClass::className()) + ": expected callback function in constructor").c_str());
					return;
				}
				obj->jscallback.Reset(v8::Local<v8::Function>::Cast(info[0]));
				info.GetReturnValue().Set(info.This());
			} else {
				const int argc = 1;
				v8::Local<v8::Value> argv[argc] = { info[0] };
				v8::Local<v8::Function> cons = Nan::New(constructor());
				info.GetReturnValue().Set(Nan::NewInstance(cons,argc, argv).ToLocalChecked());
			}
		}

		UWrap() {
			handle = -1;
		}

		~UWrap() {
			if (handle != -1) {
				uv_close(reinterpret_cast<uv_handle_t*>(&uvpoll), nullptr);
				::close(handle);
			}
		}

		void callback(string msg, v8::Local<v8::Value> a0) {
			v8::Local<v8::Value> argv[2] { Nan::New(msg.c_str()).ToLocalChecked(),  a0 };
			Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(jscallback), 2, argv);
		}

		void callback(string msg, v8::Local<v8::Value> a0, v8::Local<v8::Value> a1) {
			v8::Local<v8::Value> argv[3] { Nan::New(msg.c_str()).ToLocalChecked(),  a0, a1 };
			Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(jscallback), 3, argv);
		}

		static void poll_thunk(uv_poll_t *handle, int status, int events) {
			auto *self = static_cast<UWrap*>(handle->data);
			static_cast<SubClass*>(self)->poll(status, events);
		}

		void setupPoll() {
			int events = 0;
			if (!paused)
				events |= UV_READABLE;
			if (static_cast<SubClass*>(this)->pollWrites())
				events |= UV_WRITABLE;

			uvpoll.data = this;
			if (events == 0) {
				if (uv_poll_stop(&uvpoll) < 0) {
					callback("error", Nan::ErrnoException(-errno, "uv_poll_stop", "setupPoll", PATH_LINE()));
					return;
				}
			}
			else {
				if (uv_poll_start(&uvpoll, events, UWrap::poll_thunk) < 0) {
					callback("error", Nan::ErrnoException(-errno, "uv_poll_start", "setupPoll", PATH_LINE()));
					return;
				}
			}
		}

		void _pause() {
			assert(handle != -1);
			if (paused || handle == -1)
				return;
			paused = true;
			setupPoll();
		}

		static NAN_METHOD(pause) {
			SubClass* wrap = Nan::ObjectWrap::Unwrap<SubClass>(info.Holder());
			wrap->_pause();
		}

		void _resume() {
			assert(handle != -1);
			if (!paused || handle == -1)
				return;
			paused = false;
			setupPoll();
		}

		static NAN_METHOD(resume) {
			SubClass* wrap = Nan::ObjectWrap::Unwrap<SubClass>(info.Holder());
			wrap->_resume();
		}

		void _close() {
			jscallback.Reset();
			if (handle != -1) {
				if (uv_poll_stop(&uvpoll) < 0) {
					callback("error", Nan::ErrnoException(-errno, "uv_poll_stop", "close", PATH_LINE()));
				}
				uv_close(reinterpret_cast<uv_handle_t*>(&uvpoll), nullptr);
				::close(handle);
				handle = -1;
			}
		}

		static NAN_METHOD(close) {
			SubClass* wrap = Nan::ObjectWrap::Unwrap<SubClass>(info.Holder());
			wrap->_close();
		}

		int handle;
		Nan::Persistent<v8::Function> jscallback;
		uv_poll_t uvpoll;
		bool paused;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	struct UServerWrap : public UWrap<UServerWrap> {

		static const char * className() { return "UServerWrap"; }

		static void v8Init(v8::Handle<v8::FunctionTemplate> tpl) {
			SetPrototypeMethod(tpl, "listen", listen);
		}

		bool pollWrites() { return false; }

		void poll(int status, int events) {
			if (status < 0) {
				Nan::HandleScope scope;
				callback("error", Nan::ErrnoException(-status, "poll", "UServer socket", PATH_LINE()));
				return;
			}

			while (!paused && handle >= 0) {
				int rfd = accept(handle, nullptr, nullptr);
				if (rfd < 0) {
					int err = errno;
					if (err == EAGAIN || err == EWOULDBLOCK)
						return;
					_pause();
					Nan::HandleScope scope;
					callback("error", Nan::ErrnoException(err, "accept", "UServer socket", PATH_LINE()));
					return;
				}

				Nan::HandleScope scope;
				callback("accept", Nan::New(rfd));
			}
		}

		void _listen(string path, int backlog) {
			socketWork([path, backlog] () {
				if (path.size() >= sizeof(sockaddr_un::sun_path))
					return SocketResult("UServer socket path is too long");

				int fd = ::socket(AF_LOCAL, SOCK_STREAM, 0);
				if (fd == -1)
					return SocketResult(errno, "socket", "UServer socket", PATH_LINE());

				struct sockaddr_un addr = {};
				addr.sun_family = AF_LOCAL;
				strcpy(addr.sun_path, path.c_str());

				unlink(path.c_str());
				if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
					int err = errno;
					::close(fd);
					fd = -1;
					return SocketResult(err, "bind", "UServer socket", PATH_LINE());
				}

				if (::listen(fd, backlog) < 0) {
					int err = errno;
					::close(fd);
					fd = -1;
					return SocketResult(err, "listen", "UServer socket", PATH_LINE());
				}

				return SocketResult(fd);

			}, [this] (SocketResult result) {
				Nan::HandleScope scope;
				if (result.isError()) {
					callback("error", result.makeError());
					return;
				}

				paused = true;
				handle = result.descriptor;
				if (uv_poll_init(uv_default_loop(), &uvpoll, handle) < 0) {
					callback("error", Nan::ErrnoException(-errno, "uv_poll_init", "USocket", PATH_LINE()));
					return;
				}

				callback("listening", Nan::New(handle));
			});
		}

		static NAN_METHOD(listen) {
			UServerWrap* wrap = Nan::ObjectWrap::Unwrap<UServerWrap>(info.Holder());
			wrap->_listen(*Nan::Utf8String(info[0]), info[1]->Int32Value());
		}

		ThreadWork<SocketResult> socketWork;
	};


	//-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----
	//--

	struct USocketWrap : public UWrap<USocketWrap> {

		static const char * className() { return "USocketWrap"; }

		static void v8Init(v8::Handle<v8::FunctionTemplate> tpl) {
			SetPrototypeMethod(tpl, "connect", connect);
			SetPrototypeMethod(tpl, "adopt", adopt);
			SetPrototypeMethod(tpl, "write", write);
			SetPrototypeMethod(tpl, "shutdown", shutdown);
		}

		bool readLoop() {
			while (!paused && handle >= 0) {
				// Try to get a good sized buffer to read.
				int avail = 1024;
				if (ioctl(handle, FIONREAD, &avail) >= 0)
					avail = std::min(std::max(avail + 64, 256), 16348);

				// Build the message header.
				char ctrlBuf[CMSG_SPACE(64 * sizeof(int))];
				msghdr message = {};
				iovec iov[1];

				iov[0].iov_base = malloc(avail);
				iov[0].iov_len = avail;

				message.msg_name = nullptr;
				message.msg_namelen = 0;
				message.msg_control = ctrlBuf;
				message.msg_controllen = sizeof(ctrlBuf);
				message.msg_iov = iov;
				message.msg_iovlen = 1;

				// Try to recv a message.
				int res = recvmsg(handle, &message, 0);
				if (res < 0) {
					int err = errno;
					free(iov[0].iov_base);
					if (err == EAGAIN || err == EWOULDBLOCK)
						return true;
					_pause();
					callback("error", Nan::ErrnoException(err, "recvmesg", "USocket", PATH_LINE()));
					return false;
				}

				// Grab any file descriptors from the message.
				std::vector<int> fds;
				for (cmsghdr *c = CMSG_FIRSTHDR(&message); c != NULL; c = CMSG_NXTHDR(&message, c)) {
					if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
						fds.emplace_back();
						memcpy(&fds.back(), CMSG_DATA(c), sizeof(int));
					}
				}

				// Create a buffer of any read data.
				v8::Local<v8::Value> buffer = Nan::Undefined();
				if (res == 0 || !Nan::NewBuffer(static_cast<char*>(iov[0].iov_base), res).ToLocal(&buffer))
					free(iov[0].iov_base);

				// Convert the descriptors into a v8 array
				v8::Local<v8::Value> jsfds = Nan::Undefined();
				if (fds.size() > 0) {
					v8::Local<v8::Array> t = Nan::New<v8::Array>(fds.size());
					for (size_t i = 0; i < fds.size(); i += 1)
						t->Set(0, Nan::New(fds[i]));
					jsfds = t;
				}

				// Callback with the data.
				callback("data", buffer, jsfds);
			}

			return true;
		}

		void poll(int status, int events) {
			Nan::HandleScope scope;
			if (status < 0) {
				callback("error", Nan::ErrnoException(-status, "poll", "USocket", PATH_LINE()));
				return;
			}

			if ((events & UV_READABLE)) {
				if (!readLoop())
					return;
			}

			if ((events & UV_WRITABLE) && corked) {
				corked = false;
				setupPoll();
				callback("drain", Nan::Undefined());
			}
		}

		bool pollWrites() { return corked; }

		void _connect(string path) {
			socketWork([path] () {
				if (path.size() >= sizeof(sockaddr_un::sun_path))
					return SocketResult("USocket socket path is too long");

				int fd = ::socket(AF_LOCAL, SOCK_STREAM, 0);
				if (fd <= 0)
					return SocketResult(errno, "socket", "USocket socket", PATH_LINE());

				struct sockaddr_un addr = {};
				addr.sun_family = AF_LOCAL;
				strcpy(addr.sun_path, path.c_str());

				if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
					int err = errno;
					::close(fd);
					fd = -1;
					return SocketResult(err, "connect", "USocket socket", PATH_LINE());
				}

				return SocketResult(fd);

			}, [this] (SocketResult result) {
				Nan::HandleScope scope;
				if (result.isError()) {
					callback("error", result.makeError());
					return;
				}


				paused = true;
				corked = false;
				handle = result.descriptor;
				if (uv_poll_init(uv_default_loop(), &uvpoll, handle) < 0) {
					callback("error", Nan::ErrnoException(-errno, "uv_poll_init", "USocket", PATH_LINE()));
					return;
				}

				callback("connect", Nan::New(handle));
			});
		}

		static NAN_METHOD(connect) {
			USocketWrap* wrap = Nan::ObjectWrap::Unwrap<USocketWrap>(info.Holder());
			wrap->_connect(*Nan::Utf8String(info[0]));
		}

		void _adopt(int fd) {
			handle = fd;
			paused = true;
			corked = false;
			if (uv_poll_init(uv_default_loop(), &uvpoll, handle) < 0) {
				callback("error", Nan::ErrnoException(-errno, "uv_poll_init", "USocket", PATH_LINE()));
				return;
			}
		}

		static NAN_METHOD(adopt) {
			USocketWrap* wrap = Nan::ObjectWrap::Unwrap<USocketWrap>(info.Holder());
			wrap->_adopt(info[0]->Int32Value());
		}

		BoolResult _write(char *data, size_t len, vector<int> fds) {

			vector<char> ctrlBuf;
			msghdr message = {};
			iovec iov[1];

			message.msg_name = nullptr;
			message.msg_namelen = 0;

			if (len != 0) {
				iov[0].iov_base = data;
				iov[0].iov_len = len;
				message.msg_iov = iov;
				message.msg_iovlen = 1;
			}

			if (!fds.empty()) {
				ctrlBuf.resize(CMSG_SPACE(fds.size() * sizeof(int)));
				message.msg_control = ctrlBuf.data();
				message.msg_controllen = ctrlBuf.size();

				size_t fdi = 0;
				for (cmsghdr *c = CMSG_FIRSTHDR(&message); c != NULL; c = CMSG_NXTHDR(&message, c), fdi += 1) {
					c->cmsg_level = SOL_SOCKET;
					c->cmsg_type = SCM_RIGHTS;
					c->cmsg_len = CMSG_LEN(sizeof(int));
					memcpy(CMSG_DATA(c), &fds[fdi], sizeof(int));
	 			}
			}

			int ret = sendmsg(handle, &message, 0);
			if (ret >= 0)
				return BoolResult(true);

			int err = errno;
			if (err != EAGAIN && err != EWOULDBLOCK)
				return BoolResult(err, "sendmsg", "USocketWrap", PATH_LINE());

			corked = true;
			setupPoll();
			return BoolResult(false);
		}

		static NAN_METHOD(write) {
			USocketWrap* wrap = Nan::ObjectWrap::Unwrap<USocketWrap>(info.Holder());

			size_t len = 0;
			char *data = nullptr;
			vector<int> fds;

			if (node::Buffer::HasInstance(info[0])) {
				v8::Local<v8::Object> databuf = info[0]->ToObject();
				len = node::Buffer::Length(databuf);
				data = node::Buffer::Data(databuf);
			}

			if (info[1]->IsArray()) {
				v8::Local<v8::Array> jsfds = v8::Local<v8::Array>::Cast(info[1]);
				fds.resize(jsfds->Length());
				for (size_t i = 0; i < fds.size(); i += 1)
					fds[i] = jsfds->Get(int32_t(i))->Int32Value();
			}

			BoolResult ret = wrap->_write(data, len, move(fds));
			v8::Local<v8::Value> jsret;
			if (ret.isError())
				jsret = ret.makeError();
			else
				jsret = Nan::New(ret.ok);
			info.GetReturnValue().Set(jsret);
		}

		void _shutdown() {
			::shutdown(handle, SHUT_WR);
			corked = false;
			setupPoll();
		}

		static NAN_METHOD(shutdown) {
			USocketWrap* wrap = Nan::ObjectWrap::Unwrap<USocketWrap>(info.Holder());
			wrap->_shutdown();
		}

		bool corked;
		ThreadWork<SocketResult> socketWork;
	};


	NAN_MODULE_INIT(init) {
		UServerWrap::init(target);
		USocketWrap::init(target);
	}

} // uwrap

NODE_MODULE(uwrap, uwrap::init)
