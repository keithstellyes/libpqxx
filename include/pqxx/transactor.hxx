/* Transactor framework, a wrapper for safely retryable transactions.
 *
 * DO NOT INCLUDE THIS FILE DIRECTLY; include pqxx/transactor instead.
 *
 * Copyright (c) 2001-2017, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 */
#ifndef PQXX_H_TRANSACTOR
#define PQXX_H_TRANSACTOR

#include "pqxx/compiler-public.hxx"
#include "pqxx/compiler-internal-pre.hxx"

#include "pqxx/connection_base"
#include "pqxx/transaction"


// Methods tested in eg. test module test01 are marked with "//[t01]".

namespace pqxx
{
/**
 * @defgroup transactor Transactor framework
 *
 * Sometimes your application needs to execute a transaction that should be
 * retried if it fails.  For example, your REST API might be handling an HTTP
 * request in its own database transaction, and if it fails for transient
 * reasons, you simply want to "replay" the whole request from the start, in a
 * fresh transaction.
 *
 * One of those transient reasons might be a deadlock during a SERIALIZABLE or
 * REPEATABLE READ transaction.  Another reason might be that your network
 * connection to the database fails, and perhaps you don't just want to give up
 * when that happens.
 *
 * In situations like these, the right thing to do is often to restart your
 * transaction from scratch.  You won't necessarily want to execute the exact
 * same SQL commands with the exact same data, but you'll want to re-run the
 * same application code that produced those SQL commands.
 *
 * The transactor framework makes it a little easier for you to do this safely,
 * and avoid typical pitfalls.  You encapsulate the work that you want to do in
 * a transaction into something that you pass to @c perform.
 *
 * Transactors come in two flavours.
 *
 * The old, pre-C++11 way is to derive a class from the @c transactor template,
 * and pass an instance of it to your connection's @c connection_base::perform
 * member function.  That function will create a transaction object and pass
 * it to your @c transactor, handle any exceptions, commit or abort, and
 * repeat as appropriate.
 *
 * The new, simpler C++11-based way is to write your transaction code as a
 * lambda (or other callable), which creates its own transaction object, does
 * its work, and commits at the end.  You pass that callback to pqxx::perform.
 * If any given attempt fails, its transaction object goes out of scope and
 * gets destroyed, so that it aborts implicitly.  Your callback can return its
 * results to the calling code.
 */
//@{

/// Simple way to execute a transaction with automatic retry.
/**
 * Executes your transaction code as a callback.  Repeats it until it completes
 * normally, or it throws an error other than the few libpqxx-generated
 * exceptions that the framework understands, or after a given number of failed
 * attempts, or if the transaction ends in an "in-doubt" state.
 *
 * (An in-doubt state is one where libpqxx cannot determine whether the server
 * finally committed a transaction or not.  This can happen if the network
 * connection to the server is lost just while we're waiting for its reply to
 * a "commit" statement.  The server may have completed the commit, or not, but
 * it can't tell you because there's no longer a connection.
 *
 * Using this still takes a bit of care.  If your callback makes use of data
 * from the database, you'll probably have to query that data within your
 * callback.  If the attempt to perform your callback fails, and the framework
 * tries again, you'll be in a new transaction and the data in the database may
 * have changed under your feet.
 *
 * Also be careful about changing variables or data structures from within
 * your callback.  The run may still fail, and perhaps get run again.  The
 * ideal way to do it (in most cases) is to return your result from your
 * callback, and change your program's data after @c perform completes
 * successfully.
 *
 * This function replaces an older, more complicated transactor framework.
 * The new function is a simpler, more lambda-friendly way of doing the same
 * thing.
 *
 * @param callback Transaction code that can be called with no arguments.
 * @param attempts Maximum number of times to attempt performing callback.
 *	Must be greater than zero.
 * @return Whatever your callback returns.
 */
template<typename TRANSACTION_CALLBACK>
inline auto perform(const TRANSACTION_CALLBACK &callback, int attempts=3)
  -> decltype(callback())
{
  if (attempts <= 0)
    throw std::invalid_argument(
	"Zero or negative number of attempts passed to pqxx::perform().");

  for (; attempts > 0; --attempts)
  {
    try
    {
      return callback();
    }
    catch (const in_doubt_error &)
    {
      // Not sure whether transaction went through or not.  The last thing in
      // the world that we should do now is try again!
      throw;
    }
    catch (const statement_completion_unknown &)
    {
      // Not sure whether our last statement succeeded.  Don't risk running it
      // again.
      throw;
    }
    catch (const broken_connection &)
    {
      // Connection failed.  Definitely worth retrying.
      if (attempts <= 1) throw;
      continue;
    }
    catch (const transaction_rollback &)
    {
      // Some error that may well be transient, such as serialization failure
      // or deadlock.  Worth retrying.
      if (attempts <= 1) throw;
      continue;
    }
  }
  throw pqxx::internal_error("No outcome reached on perform().");
}

/// @deprecated Pre-C++11 wrapper for automatically retrying transactions.
/**
 * Pass an object of your transactor-based class to connection_base::perform()
 * to execute the transaction code embedded in it.
 *
 * connection_base::perform() is actually a template, specializing itself to any
 * transactor type you pass to it.  This means you will have to pass it a
 * reference of your object's ultimate static type; runtime polymorphism is
 * not allowed.  Hence the absence of virtual methods in transactor.  The
 * exact methods to be called at runtime *must* be resolved at compile time.
 *
 * Your transactor-derived class must define a copy constructor.  This will be
 * used to create a "clean" copy of your transactor for every attempt that
 * perform() makes to run it.
 */
template<typename TRANSACTION=transaction<read_committed>> class transactor
{
public:
  using argument_type = TRANSACTION;
  explicit transactor(const std::string &TName="transactor") :		//[t04]
    m_name(TName) { }

  /// Overridable transaction definition; insert your database code here
  /** The operation will be retried if the connection to the backend is lost or
   * the operation fails, but not if the connection is broken in such a way as
   * to leave the library in doubt as to whether the operation succeeded.  In
   * that case, an in_doubt_error will be thrown.
   *
   * Recommended practice is to allow this operator to modify only the
   * transactor itself, and the dedicated transaction object it is passed as an
   * argument.  This is what makes side effects, retrying etc. controllable in
   * the transactor framework.
   * @param T Dedicated transaction context created to perform this operation.
   */
  void operator()(TRANSACTION &T);					//[t04]

  // Overridable member functions, called by connection_base::perform() if an
  // attempt to run transaction fails/succeeds, respectively, or if the
  // connection is lost at just the wrong moment, goes into an indeterminate
  // state.  Use these to patch up runtime state to match events, if needed, or
  // to report failure conditions.

  /// Optional overridable function to be called if transaction is aborted
  /** This need not imply complete failure; the transactor will automatically
   * retry the operation a number of times before giving up.  on_abort() will be
   * called for each of the failed attempts.
   *
   * One parameter is passed in by the framework: an error string describing why
   * the transaction failed.  This will also be logged to the connection's
   * notice processor.
   */
  void on_abort(const char[]) noexcept {}				//[t13]

  /// Optional overridable function to be called after successful commit
  /** If your on_commit() throws an exception, the actual back-end transaction
   * will remain committed, so any changes in the database remain regardless of
   * how this function terminates.
   */
  void on_commit() {}							//[t07]

  /// Overridable function to be called when "in doubt" about outcome
  /** This may happen if the connection to the backend is lost while attempting
   * to commit.  In that case, the backend may have committed the transaction
   * but is unable to confirm this to the frontend; or the transaction may have
   * failed, causing it to be rolled back, but again without acknowledgement to
   * the client program.  The best way to deal with this situation is typically
   * to wave red flags in the user's face and ask him to investigate.
   *
   * The robusttransaction class is intended to reduce the chances of this
   * error occurring, at a certain cost in performance.
   * @see robusttransaction
   */
  void on_doubt() noexcept {}						//[t13]

  /// The transactor's name.
  std::string name() const { return m_name; }				//[t13]

private:
  std::string m_name;
};


template<typename TRANSACTOR>
inline void connection_base::perform(
	const TRANSACTOR &T,
        int Attempts)
{
  if (Attempts <= 0) return;

  bool Done = false;

  // Make attempts to perform T
  do
  {
    --Attempts;

    // Work on a copy of T2 so we can restore the starting situation if need be
    TRANSACTOR T2(T);
    try
    {
      typename TRANSACTOR::argument_type X(*this, T2.name());
      T2(X);
      X.commit();
      Done = true;
    }
    catch (const in_doubt_error &)
    {
      // Not sure whether transaction went through or not.  The last thing in
      // the world that we should do now is retry.
      T2.on_doubt();
      throw;
    }
    catch (const std::exception &e)
    {
      // Could be any kind of error.
      T2.on_abort(e.what());
      if (Attempts <= 0) throw;
      continue;
    }
    catch (...)
    {
      // Don't try to forge ahead if we don't even know what happened
      T2.on_abort("Unknown exception");
      throw;
    }

    T2.on_commit();
  } while (!Done);
}
} // namespace pqxx
//@}
#include "pqxx/compiler-internal-post.hxx"
#endif
