// X11Wrapper.hpp

#pragma once
#ifndef X11Wrapper_HEADER
#define X11Wrapper_HEADER

#include <string>
#include <string_view>
#include <stdexcept>
#include <future>
#include <mutex>
#include <condition_variable>
#include <type_traits>
#include <chrono>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <boost/asio.hpp>

namespace X11Wrapper {
    // source: https://stackoverflow.com/questions/27378318/c-get-string-from-clipboard-on-linux 
    inline std::string readImpl()
    {
        Display* display = NULL;
        char* display_name = NULL;
        const char* bufname = "CLIPBOARD";
        const char* fmtname = "UTF8_STRING";
        if(!(display = XOpenDisplay(display_name))) {
            throw std::runtime_error("Error XOpenDisplay\n");
        }
        unsigned long color = BlackPixel(display, DefaultScreen(display));
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, color, color);
        char *result;
        unsigned long ressize, restail;
        int resbits;
        Atom bufid = XInternAtom(display, bufname, False),
            fmtid = XInternAtom(display, fmtname, False),
            propid = XInternAtom(display, "XSEL_DATA", False),
            incrid = XInternAtom(display, "INCR", False);
        XEvent event;
        XConvertSelection(display, bufid, fmtid, propid, window, CurrentTime);
        do {
            XNextEvent(display, &event);
        } while (event.type != SelectionNotify || event.xselection.selection != bufid);
        if (event.xselection.property) {
            XGetWindowProperty(
                display, 
                window, 
                propid, 
                0, 
                LONG_MAX / 4, 
                False, 
                AnyPropertyType,
                &fmtid, 
                &resbits, 
                &ressize, 
                &restail, 
                (unsigned char**)&result
            );
            if (fmtid == incrid) {
                XDestroyWindow(display, window);
                XCloseDisplay(display);
                throw std::runtime_error("Buffer is too large and INCR reading is not implemented yet.\n");
            }
            std::string res(result, ressize);
            XFree(result);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            return res;
        }
        else {
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            throw std::runtime_error("request failed, e.g. owner can't convert to the target format");   
        }
    }

    class ClipboardWriter {
    public:
        ClipboardWriter(boost::asio::io_context & ioc) :
            m_strand(ioc),
            m_stream_descriptor(ioc)
        {
            m_display = XOpenDisplay(NULL);
            if (!m_display) {
                throw std::runtime_error("Could not open X display");
            }

            m_screen = DefaultScreen(m_display);
            m_root = RootWindow(m_display, m_screen);

            /* We need a window to receive messages from other clients. */
            m_owner = XCreateSimpleWindow(m_display, m_root, -10, -10, 1, 1, 0, 0, 0);

            m_sel = XInternAtom(m_display, "CLIPBOARD", False);
            m_utf8 = XInternAtom(m_display, "UTF8_STRING", False);
            m_string = XInternAtom(m_display, "STRING", False);
            m_targets = XInternAtom(m_display, "TARGETS", False);
            
            int fd = ConnectionNumber(m_display);
            if (fd < 0) {
                throw std::runtime_error("Invalid fd");
            }
            m_stream_descriptor = boost::asio::posix::basic_stream_descriptor(m_stream_descriptor.get_executor(), fd);
        }

        template <class Handler>
        void close(Handler&& handler)
        {
            boost::asio::post(m_stream_descriptor.get_executor(), boost::asio::bind_executor(m_strand, [handler = std::forward<Handler>(handler), this] {
                killX11();    
                m_stream_descriptor.cancel();
                handler();
            }));
        }

        void close()
        {
            close([](){});
        }

        template <class Handler>
        void setMsg(std::string&& msg, Handler&& handler)
        {
            boost::asio::post(m_stream_descriptor.get_executor(), boost::asio::bind_executor(m_strand, [msg = std::move(msg), handler = std::move(handler), this]() {
                m_stream_descriptor.cancel();
                m_msg = std::move(msg);
                aquireX11SelectionOwnership(std::move(handler));
            }));
        }

        void setMsg(std::string&& msg)
        {
            setMsg(std::move(msg), [](){});
        }

    private:
        Display* m_display;
        Window m_owner;
        Window m_root;
        int m_screen;
        Atom m_sel;
        Atom m_utf8;
        Atom m_string;
        Atom m_targets;
        boost::asio::io_context::strand m_strand;
        boost::asio::posix::stream_descriptor m_stream_descriptor;
        std::string m_msg;
    
        template <class Handler>
        void aquireX11SelectionOwnership(Handler&& handler)
        {
            assert(m_strand.running_in_this_thread());
            assert(m_stream_descriptor.is_open());

            /* Claim ownership of the clipboard. */
            XSetSelectionOwner(m_display, m_sel, m_owner, CurrentTime);
            m_stream_descriptor.async_wait(m_stream_descriptor.wait_write, boost::asio::bind_executor(m_strand, 
                [handler = std::move(handler), this](const boost::system::error_code& error){
                    writeHandler(error, std::move(handler));
                }
            ));
        }

        void killX11()
        {
            XDestroyWindow(m_display, m_owner);
            XDestroyWindow(m_display, m_root);
            XCloseDisplay(m_display);
        }

        template <class Handler>
        void writeHandler(const boost::system::error_code& error, Handler&& handler)
        {
            if (error == boost::asio::error::operation_aborted) {
                return;
            }
            else if (error) {
                return;
            }
            
            while (XPending(m_display)) {
                XEvent ev;
                XNextEvent(m_display, &ev);
            }
            
            m_stream_descriptor.async_wait(m_stream_descriptor.wait_read, boost::asio::bind_executor(m_strand, 
                [handler = std::move(handler), this](const boost::system::error_code& error) {
                    readHandler(error, std::move(handler));
                }
            ));
        }

        template <class Handler>
        void readHandler(const boost::system::error_code& error, Handler&& handler)
        {
            if (error == boost::asio::error::operation_aborted) {
                return;
            }
            else if (error) {
                return;
            }
            while (XPending(m_display)) {
                XEvent ev;
                XNextEvent(m_display, &ev);
                if (ev.type == SelectionClear) {
                    return;
                }
                else if (ev.type == SelectionRequest) {
                    XSelectionRequestEvent* sev = (XSelectionRequestEvent*)&ev.xselectionrequest;
                    if (sev->target == m_utf8) {
                        sendUtf8(sev, m_utf8, m_msg);
                    }
                    else if (sev->target == m_string) {
                        sendString(sev, m_string, m_msg);
                    }
                    else if (sev->target == m_targets) {
                        sendTargets(sev);
                    }
                    else {
                        sendNo(sev);
                    }
                }
            }
            m_stream_descriptor.async_wait(m_stream_descriptor.wait_read, boost::asio::bind_executor(m_strand, 
                [handler = std::move(handler), this](const boost::system::error_code& error){
                    readHandler(error, std::move(handler));
                }
            ));
        }

        void sendTargets(XSelectionRequestEvent * sev)
        {
            const Atom targets[] = {
                m_utf8,
                m_string,
                m_targets
            };
            
            XSelectionEvent ssev;
            time_t now_tm;
            char *now, *an;
            an = XGetAtomName(m_display, sev->property);
            if (an) {
                XFree(an);
            }
            XChangeProperty(m_display, sev->requestor, sev->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)targets, sizeof(targets) / sizeof(targets[0]));
            ssev.type = SelectionNotify;
            ssev.requestor = sev->requestor;
            ssev.selection = sev->selection;
            ssev.target = sev->target;
            ssev.property = sev->property;
            ssev.time = sev->time;
            XSendEvent(m_display, sev->requestor, True, NoEventMask, (XEvent*)&ssev);
        }

        // source: https://www.uninformativ.de/blog/postings/2017-04-02/0/POSTING-en.html
        void sendNo(XSelectionRequestEvent *sev)
        {
            XSelectionEvent ssev;
            char *an;
            an = XGetAtomName(m_display, sev->target);
            if (an)
                XFree(an);

            /* All of these should match the values of the request. */
            ssev.type = SelectionNotify;
            ssev.requestor = sev->requestor;
            ssev.selection = sev->selection;
            ssev.target = sev->target;
            ssev.property = None;  /* signifies "nope" */
            ssev.time = sev->time;
            XSendEvent(m_display, sev->requestor, True, NoEventMask, (XEvent *)&ssev);
        }

        void sendUtf8(XSelectionRequestEvent *sev, Atom utf8, const std::string & msg)
        {
            XSelectionEvent ssev;
            time_t now_tm;
            char *now, *an;
            an = XGetAtomName(m_display, sev->property);
            if (an) {
                XFree(an);
            }
            XChangeProperty(m_display, sev->requestor, sev->property, utf8, 8, PropModeReplace, (unsigned char *)msg.data(), msg.size());
            ssev.type = SelectionNotify;
            ssev.requestor = sev->requestor;
            ssev.selection = sev->selection;
            ssev.target = sev->target;
            ssev.property = sev->property;
            ssev.time = sev->time;
            XSendEvent(m_display, sev->requestor, True, NoEventMask, (XEvent*)&ssev);
        }

        void sendString(XSelectionRequestEvent *sev, Atom string, const std::string & msg)
        {
            XSelectionEvent ssev;
            time_t now_tm;
            char *now, *an;
            an = XGetAtomName(m_display, sev->property);
            if (an) {
                XFree(an);
            }
            XChangeProperty(m_display, sev->requestor, sev->property, string, 8, PropModeReplace, (unsigned char *)msg.data(), msg.size());
            ssev.type = SelectionNotify;
            ssev.requestor = sev->requestor;
            ssev.selection = sev->selection;
            ssev.target = sev->target;
            ssev.property = sev->property;
            ssev.time = sev->time;
            XSendEvent(m_display, sev->requestor, True, NoEventMask, (XEvent*)&ssev);
        }
    };
}

#endif
