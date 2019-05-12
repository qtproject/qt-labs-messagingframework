/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QLOGGERS_H
#define QLOGGERS_H

#include <qlogsystem.h>
#include <qglobal.h>
#if !defined(Q_OS_WIN)
#include <syslog.h>
#endif

/*!
 * \brief The very basic foundations of the "base" logger
 *
 * This is the non-template part of BaseLogger in fact. See documentation on BaseLogger for more details.
*/
class QMF_EXPORT BaseLoggerFoundation : public ILogger
{
public:
        explicit BaseLoggerFoundation(const LogLevel min_lvl_ = LlInfo);

         /*!
          \brief Sets minimal log level. Messages with log level less than minimum log level will not be logged.

          \param min_lvl New value of minimal log level.
        */
        void setMinLogLvl(const LogLevel _minlvl);

        /// Returns the minimal log level set for the logger
        LogLevel getMinLogLvl() const { return min_lvl; }

        /*!
          \brief Is this logger ready for logging?

          \param err Reference to string where error message should be placed in case where logger is not ready.

          \return true if logger is ready for logging, otherwise returns false and sets err variable to human-readable
                  error message.
        */
        bool isReady(QString& err) const;

        /*!
          \brief Is this logger ready for logging?

          \return true if logger is ready for logging, otherwise returns false.
        */
        bool isReady() const { return is_ready; }

        /// Empty destructor
        virtual ~BaseLoggerFoundation()  { }

protected:
        /*!
          \brief Set "ready for the logging" status to false

                 This method should be called by host whenever critical error with loging device occurs.

          \param err Text error message.
        */
        void setUnReady(const QString& err);
        /*!
          \brief Set "ready for the logging" status to true

        */
        void setReady(void);

private:
        /// Minimal log level.
        LogLevel min_lvl;
        /// Are we ready for the mission (logging)?
        bool is_ready;
        /// Error message if is_ready equals to false
        QString err_msg;
};


/*!
  \brief Base (and trivial) logger functionality

  This template is used to simplify loggers declarations and implementations. The main logic it implements is
  the "minimal log level" logic.  This template can not be used alone.  It should be used only with host class.

  Host's member function void doLog(const char* fmt, va_list args) will be called by
  BaseLogger::operator()(LogLevel lvl, const char* fmt, va_list args) if the lvl argument is greater or equal than
  minimal log level.

  Loggers are frequently used as singletons in the modern world of software design.  That's why logger's constructors
  can be called before entering main() function. So, it would not be a perfect design style to throw exceptions in logger's
  constructors. On the other hand, there is a common solution to initialize logger resources inside its constructors.  It is
  clear, that such initialization procedure can fail. It is necessary to know the result of initialization after entering
  main().  #isReady/#setUnReady methods pair of this class seems to be the solution of this dilemma.  #isReady is public
  method which should be used to check loggers's "ready for the mission" (successfull initialization) status.
  #SetUnReady is protected method which should be called by derived class's constructors instead of exception throwing.
*/
template <class Host, class Prefix = NoLogPrefix> class BaseLogger: public BaseLoggerFoundation
{
public:
        /*!
          \brief  Stores reference to host (to make possible host.OutMsg call in operator()) and
                  minimal log level. Messages with log level less than minimum log level will not be logged.

          \param  host		Reference to host object
          \param  min_lvl	Minimal log level
        */
        BaseLogger(Host& host, const LogLevel min_lvl = LlInfo);

        /// Standard destructor
        virtual ~BaseLogger()
        {
        }

        /*!
          \brief  Log function. Called by LogSystem::log if the logger is added to the LogSystem

                  Checks lvl and if it is greater or equal then minumal log level, calls host's doLog function.

          \param lvl  Log-level. Messages with log level less than minimum log level will not be logged
          \param fmt  Format of log string.  See (v)printf manual for details.
          \param args Arguments related to format.  See vprintf manual for additional information.
        */
        void log(const LogLevel lvl, const char* fmt, va_list args);

private:
        /// Reference to host object
        Host& host;
        /// Prefix generator
        Prefix prefix;
        /// Bufer used to write log-level text presentation into output log device
        QString buf;
};

/*!
  \brief Logging device - file

  This class can be used for logging into files. See also documentation on BaseLogger for additional useful remarks.
*/
template <class Prefix = LvlTimeLogPrefix>
class FileLogger : public BaseLogger<FileLogger<Prefix>, Prefix>
{
public:
        /*!
          \brief  Opens log file in append mode.  Any error can be checked via isReady method.

          \param name 		The name of log file.
          \param flush_period  Flush period (in lines)
          \param min_lvl 	Minimal log level.  Messages with log level less than _min_lvl will not be logged
        */
        FileLogger(const QString& name, const unsigned flush_period = 10, const LogLevel min_lvl = LlInfo);
        /*!
          \brief This constructor is for logging into already open files.  stderr and stdout files are typical
                 examples.

          \param f		Pointer to the open file. It should be writable, of course.
          \param flush_period  Flush period (in lines)
          \param min_lvl	Minimal log level.  Messages with log level less than _min_lvl will not be logged.
          \param owner 	File closing policy.  If _owner is false file _f will not be closed in FileLogger's destructor, else will be.
        */
        FileLogger(FILE* f, const unsigned flush_period = 10, const LogLevel min_lvl = LlInfo, bool owner = false);
        /*!
          \brief  Real logging function.  See BaseLogger documentation for additional information.

                  Flushes the output every period (see documentation on FileLogger constructors for additional details)

          \param lvl  Log-level. Messages with log level less than minimum log level will not be logged
          \param fmt  Format of log string.  See (v)printf manual for details.
          \param args Arguments related to format.  See vprintf manual for additional information.
        */
        virtual void doLog(const LogLevel lvl, const char* fmt, va_list args);
        /// Closes file if this object is it's owner.
        virtual ~FileLogger();
private:
        Q_DISABLE_COPY(FileLogger)
        QString name;
        /// File handle
        FILE* f;
        /// Should I close file in destructor?
        bool should_close;
        /// Counter of "doLog" calls
        unsigned do_cntr;
        /// Flush period in lines
        const unsigned flush_period;
};

/*!
  \class SysLogger
  \brief Logging device - syslog.

  This class can be used for logging via syslog. See also documentation on #BaseLogger for additional useful remarks.
*/
template <class Prefix = LvlTimePidLogPrefix>
class SysLogger : public BaseLogger<SysLogger<Prefix>, Prefix>
{
public:
        /*!
          \brief Opens a connection to system logger

          \param ident		See openlog manual
          \param option	See openlog manual
          \param facility	See openlog manual
          \param min_lvl	Minimal log level.  Messages with log level less than _min_lvl will not be logged
        */
        SysLogger(const QString& ident, int option, int facility, const LogLevel min_lvl = LlInfo);
        /*!
          \brief  Real logging function.  See BaseLogger documentation for additional information.

          \param lvl  Log-level. Messages with log level less than minimum log level will not be logged
          \param fmt  Format of log string.  See (v)printf manual for details.
          \param args Arguments related to format.  See vprintf manual for additional information.
        */
        virtual void doLog(const LogLevel lvl, const char* fmt, va_list args);
        /// Disconnects from the syslog
        virtual ~SysLogger();
private:
        Q_DISABLE_COPY(SysLogger)
        /// Ident string for syslog
        QByteArray ident;
};

/**********************************************************************************************************/
/*********************************** BaseLogger implementation ********************************************/
/**********************************************************************************************************/

template <class Host, class Prefix>
BaseLogger<Host, Prefix>::BaseLogger(Host& _host, const LogLevel _min_lvl)
                 : BaseLoggerFoundation(_min_lvl), host(_host)
{
}

template <class Host, class Prefix>
inline void BaseLogger<Host, Prefix>::log(const LogLevel _lvl, const char* _fmt, va_list args)
{
    if(_lvl >= getMinLogLvl())
    {
        Q_ASSERT(isReady());

        const QString& pref = prefix(_lvl);

        if(!pref.isEmpty())
        {
            QString out = pref + QString::fromLatin1(_fmt);
            host.doLog(_lvl, qPrintable(out), args);
        }else
            host.doLog(_lvl, _fmt, args);


   }
}

/**********************************************************************************************************/
/*********************************** FileLogger implementation ********************************************/
/**********************************************************************************************************/

template <class Prefix>
inline FileLogger<Prefix>::FileLogger(const QString& _name, const unsigned _flush_period, const LogLevel _min_lvl)
           : BaseLogger< FileLogger<Prefix>, Prefix >(*this, _min_lvl), name(_name), should_close(true), do_cntr(0), flush_period(_flush_period)
{
    f = fopen(qPrintable(_name), "a" );
    if(f == NULL)
    {
        should_close = false;
        BaseLogger< FileLogger<Prefix>, Prefix >::setUnReady(QString::fromUtf8(strerror(errno)));
    }
}

template <class Prefix>
inline FileLogger<Prefix>::FileLogger(FILE* _f, const unsigned _flush_period, const LogLevel _min_lvl, bool _owner)
    : BaseLogger< FileLogger<Prefix>, Prefix >(*this, _min_lvl), name(), f(_f), should_close(_owner), flush_period(_flush_period)
{
}

template <class Prefix>
inline FileLogger<Prefix>::~FileLogger()
{
    if(should_close)
        fclose(f);
}

template <class Prefix>
inline void FileLogger<Prefix>::doLog(const LogLevel /*lvl*/, const char* fmt, va_list args)
{
    vfprintf(f, fmt, args);
    fprintf(f, "\n");

    if(++do_cntr > flush_period)
    {
        do_cntr = 0;
        fflush(f);
    };
}

#if !defined(Q_OS_WIN)

/**********************************************************************************************************/
/************************************ SysLogger implementation ********************************************/
/**********************************************************************************************************/

template <class Prefix>
inline SysLogger<Prefix>::SysLogger(const QString& _ident, int _option, int _facility, const LogLevel  _min_lvl)
    :BaseLogger<SysLogger<Prefix>, Prefix>(*this, _min_lvl), ident(_ident.toLatin1())
{
    openlog(ident.data(), _option, _facility);
}

template <class Prefix>
inline void SysLogger<Prefix>::doLog(const LogLevel _lvl, const char* _fmt, va_list _args)
{
    static int priorities[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR,  LOG_CRIT };
    Q_ASSERT((_lvl >= LlDbg) && (_lvl <= LlCritical));

    vsyslog(priorities[_lvl], _fmt, _args);
}

template <class Prefix>
inline SysLogger<Prefix>::~SysLogger()
{
    closelog();
}

#endif // Q_OS_WIN

#endif // QLOGGERS_H
