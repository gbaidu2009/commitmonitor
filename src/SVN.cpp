#include "StdAfx.h"
#include "svn.h"
#include "svn_sorts.h"

#include "TempFile.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

SVN::SVN(void)
{
	parentpool = svn_pool_create(NULL);
	svn_client_create_context(&m_pctx, parentpool);

	Err = svn_config_ensure(NULL, parentpool);
	pool = svn_pool_create (parentpool);
	// set up the configuration
	if (Err == 0)
		Err = svn_config_get_config (&(m_pctx->config), NULL, parentpool);

	if (Err == 0)
	{
		//set up the SVN_SSH param
		stdstring tsvn_ssh = CRegStdString(_T("Software\\TortoiseSVN\\SSH"));
		if (!tsvn_ssh.empty())
		{
			svn_config_t * cfg = (svn_config_t *)apr_hash_get (m_pctx->config, SVN_CONFIG_CATEGORY_CONFIG,
				APR_HASH_KEY_STRING);
			svn_config_set(cfg, SVN_CONFIG_SECTION_TUNNELS, "ssh", CUnicodeUtils::StdGetUTF8(tsvn_ssh).c_str());
		}
	}

	Err = svn_ra_initialize(parentpool);

	// set up authentication
	svn_auth_provider_object_t *provider;

	/* The whole list of registered providers */
	apr_array_header_t *providers = apr_array_make (pool, 10, sizeof (svn_auth_provider_object_t *));

	/* The main disk-caching auth providers, for both
	'username/password' creds and 'username' creds.  */
	svn_auth_get_windows_simple_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	svn_auth_get_simple_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	svn_auth_get_username_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	/* The server-cert, client-cert, and client-cert-password providers. */
	svn_auth_get_windows_ssl_server_trust_provider(&provider, pool); 
	APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;
	svn_auth_get_ssl_server_trust_file_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	svn_auth_get_ssl_client_cert_file_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	svn_auth_get_ssl_client_cert_pw_file_provider (&provider, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	/* Two prompting providers, one for username/password, one for
	just username. */
	//svn_auth_get_simple_prompt_provider (&provider, (svn_auth_simple_prompt_func_t)simpleprompt, this, 3, /* retry limit */ pool);
	//APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	//svn_auth_get_username_prompt_provider (&provider, (svn_auth_username_prompt_func_t)userprompt, this, 3, /* retry limit */ pool);
	//APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	/* Three prompting providers for server-certs, client-certs,
	and client-cert-passphrases.  */
	svn_auth_get_ssl_server_trust_prompt_provider (&provider, sslserverprompt, this, pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	//svn_auth_get_ssl_client_cert_prompt_provider (&provider, sslclientprompt, this, 2, pool);
	//APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
	//svn_auth_get_ssl_client_cert_pw_prompt_provider (&provider, sslpwprompt, this, 2, pool);
	//APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	/* Build an authentication baton to give to libsvn_client. */
	svn_auth_open (&auth_baton, providers, pool);
	svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_NON_INTERACTIVE, "");
	svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_DONT_STORE_PASSWORDS, "");
	svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_NO_AUTH_CACHE, "");

	m_pctx->auth_baton = auth_baton;
	m_pctx->cancel_func = cancel;
	m_pctx->cancel_baton = this;
}

SVN::~SVN(void)
{
	svn_pool_destroy (parentpool);
}

svn_error_t* SVN::cancel(void *baton)
{
	UNREFERENCED_PARAMETER(baton);
	return SVN_NO_ERROR;
}

svn_error_t* SVN::sslserverprompt(svn_auth_cred_ssl_server_trust_t **cred_p, void *baton, 
								  const char *realm, apr_uint32_t failures, 
								  const svn_auth_ssl_server_cert_info_t *cert_info, 
								  svn_boolean_t may_save, apr_pool_t *pool)
{
	*cred_p = (svn_auth_cred_ssl_server_trust_t*)apr_pcalloc (pool, sizeof (**cred_p));
	(*cred_p)->may_save = FALSE;
	return SVN_NO_ERROR;
}


void SVN::SetAuthInfo(const stdstring& username, const stdstring& password)
{
	if (m_pctx)
	{
		if (!username.empty())
		{
			svn_auth_set_parameter(m_pctx->auth_baton, 
				SVN_AUTH_PARAM_DEFAULT_USERNAME, apr_pstrdup(parentpool, CUnicodeUtils::StdGetUTF8(username).c_str()));
			svn_auth_set_parameter(m_pctx->auth_baton, 
				SVN_AUTH_PARAM_DEFAULT_PASSWORD, apr_pstrdup(parentpool, CUnicodeUtils::StdGetUTF8(password).c_str()));
		}
	}
}

bool SVN::Cat(stdstring sUrl, stdstring sFile)
{
	// we always use the HEAD revision to fetch a file
	apr_file_t * file;
	svn_stream_t * stream;
	apr_status_t status;
	SVNPool localpool(pool);

	// if the file already exists, delete it before recreating it
	::DeleteFile(sFile.c_str());

	status = apr_file_open(&file, CUnicodeUtils::StdGetANSI(sFile).c_str(), 
		APR_WRITE | APR_CREATE | APR_TRUNCATE, APR_OS_DEFAULT, localpool);
	if (status)
	{
		Err = svn_error_wrap_apr(status, NULL);
		return false;
	}
	stream = svn_stream_from_aprfile(file, localpool);

	svn_opt_revision_t pegrev, rev;
	pegrev.kind = svn_opt_revision_head;
	rev.kind = svn_opt_revision_head;

	const char * urla = svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(sUrl).c_str(), localpool);
	Err = svn_client_cat2(stream, urla, 
		&pegrev, &rev, m_pctx, localpool);

	apr_file_close(file);

	return (Err == NULL);
}

const SVNInfoData * SVN::GetFirstFileInfo(stdstring path, svn_revnum_t pegrev, svn_revnum_t revision, bool recurse /* = false */)
{
	SVNPool localpool(pool);
	m_arInfo.clear();
	m_pos = 0;
	
	svn_opt_revision_t peg, rev;
	if (pegrev == -1)
		peg.kind = svn_opt_revision_head;
	else
	{
		peg.kind = svn_opt_revision_number;
		peg.value.number = pegrev;
	}
	if (revision == -1)
		rev.kind = svn_opt_revision_head;
	else
	{
		rev.kind = svn_opt_revision_number;
		rev.value.number = revision;
	}

	const char * urla = svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(path).c_str(), localpool);

	Err = svn_client_info(urla, &peg, &rev, infoReceiver, this, recurse, m_pctx, localpool);
	if (Err != NULL)
		return NULL;
	if (m_arInfo.size() == 0)
		return NULL;
	return &m_arInfo[0];
}

const SVNInfoData * SVN::GetNextFileInfo()
{
	m_pos++;
	if (m_arInfo.size()>m_pos)
		return &m_arInfo[m_pos];
	return NULL;
}

svn_error_t * SVN::infoReceiver(void* baton, const char * path, const svn_info_t* info, apr_pool_t * /*pool*/)
{
	if ((path == NULL)||(info == NULL))
		return NULL;

	SVN * pThis = (SVN *)baton;

	SVNInfoData data;
	if (info->URL)
		data.url = CUnicodeUtils::StdGetUnicode(info->URL);
	data.rev = info->rev;
	data.kind = info->kind;
	if (info->repos_root_URL)
		data.reposRoot = CUnicodeUtils::StdGetUnicode(info->repos_root_URL);
	if (info->repos_UUID)
		data.reposUUID = CUnicodeUtils::StdGetUnicode(info->repos_UUID);
	data.lastchangedrev = info->last_changed_rev;
	data.lastchangedtime = info->last_changed_date/1000000L;
	if (info->last_changed_author)
		data.author = CUnicodeUtils::StdGetUnicode(info->last_changed_author);

	if (info->lock)
	{
		if (info->lock->path)
			data.lock_path = CUnicodeUtils::StdGetUnicode(info->lock->path);
		if (info->lock->token)
			data.lock_token = CUnicodeUtils::StdGetUnicode(info->lock->token);
		if (info->lock->owner)
			data.lock_owner = CUnicodeUtils::StdGetUnicode(info->lock->owner);
		if (info->lock->comment)
			data.lock_comment = CUnicodeUtils::StdGetUnicode(info->lock->comment);
		data.lock_davcomment = !!info->lock->is_dav_comment;
		data.lock_createtime = info->lock->creation_date/1000000L;
		data.lock_expirationtime = info->lock->expiration_date/1000000L;
	}

	data.hasWCInfo = !!info->has_wc_info;
	if (info->has_wc_info)
	{
		data.schedule = info->schedule;
		if (info->copyfrom_url)
			data.copyfromurl = CUnicodeUtils::StdGetUnicode(info->copyfrom_url);
		data.copyfromrev = info->copyfrom_rev;
		data.texttime = info->text_time/1000000L;
		data.proptime = info->prop_time/1000000L;
		if (info->checksum)
			data.checksum = CUnicodeUtils::StdGetUnicode(info->checksum);
		if (info->conflict_new)
			data.conflict_new = CUnicodeUtils::StdGetUnicode(info->conflict_new);
		if (info->conflict_old)
			data.conflict_old = CUnicodeUtils::StdGetUnicode(info->conflict_old);
		if (info->conflict_wrk)
			data.conflict_wrk = CUnicodeUtils::StdGetUnicode(info->conflict_wrk);
		if (info->prejfile)
			data.prejfile = CUnicodeUtils::StdGetUnicode(info->prejfile);
	}
	pThis->m_arInfo.push_back(data);
	return NULL;
}

svn_revnum_t SVN::GetHEADRevision(const stdstring& url)
{
	svn_ra_session_t *ra_session = NULL;
	SVNPool localpool(pool);
	svn_revnum_t rev = 0;

	// make sure the url is canonical.
	const char * urla = svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(url).c_str(), localpool);

	if (urla == NULL)
		return rev;

	Err = svn_client_open_ra_session (&ra_session, urla, m_pctx, localpool);
	if (Err)
		return rev;

	Err = svn_ra_get_latest_revnum(ra_session, &rev, localpool);

	return rev;
}

bool SVN::GetLog(const stdstring& url, svn_revnum_t startrev, svn_revnum_t endrev)
{
	SVNPool localpool(pool);

	apr_array_header_t *targets = apr_array_make (pool, 1, sizeof(const char *));
	(*((const char **) apr_array_push (targets))) = 
		svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(url).c_str(), localpool);

	svn_opt_revision_t end;
	end.kind = svn_opt_revision_number;
	end.value.number = endrev;

	svn_opt_revision_t start;
	start.kind = svn_opt_revision_number;
	start.value.number = startrev;

	m_logs.clear();

	Err = svn_client_log3 (targets, 
		&end,
		&start, 
		&end, 
		10,
		true,
		false,
		logReceiver,	// log_message_receiver
		(void *)this, m_pctx, localpool);

	return (Err == NULL);
}

svn_error_t* SVN::logReceiver(void* baton, 
							  apr_hash_t* ch_paths, 
							  svn_revnum_t rev, 
							  const char* author, 
							  const char* date, 
							  const char* msg, 
							  apr_pool_t* pool)
{
	svn_error_t * error = NULL;
	SVNLogEntry logEntry;
	SVN * svn = (SVN *)baton;

	logEntry.revision = rev;
	error = svn_time_from_cstring (&logEntry.date, date, pool);
	if (author)
		logEntry.author = CUnicodeUtils::StdGetUnicode(author);

	if (msg)
		logEntry.message = CUnicodeUtils::StdGetUnicode(msg);

	if (ch_paths)
	{
		apr_array_header_t *sorted_paths;
		sorted_paths = svn_sort__hash(ch_paths, svn_sort_compare_items_as_paths, pool);
		for (int i = 0; i < sorted_paths->nelts; i++)
		{
			SVNLogChangedPaths changedPaths;
			svn_sort__item_t *item = &(APR_ARRAY_IDX (sorted_paths, i, svn_sort__item_t));
			const char *path = (const char *)item->key;
			svn_log_changed_path_t *log_item = (svn_log_changed_path_t *)apr_hash_get (ch_paths, item->key, item->klen);
			stdstring path_native = CUnicodeUtils::StdGetUnicode(path);
			changedPaths.action = log_item->action;
			if (log_item->copyfrom_path && SVN_IS_VALID_REVNUM (log_item->copyfrom_rev))
			{
				changedPaths.copyfrom_path = CUnicodeUtils::StdGetUnicode(log_item->copyfrom_path);
				changedPaths.copyfrom_revision = log_item->copyfrom_rev;
			}
			logEntry.m_changedPaths[path_native] = changedPaths;
		}
	}
	svn->m_logs[rev] = logEntry;

	return error;
}

bool SVN::Diff(const wstring& url1, svn_revnum_t revision1, const wstring& url2, 
			   svn_revnum_t revision2, bool ignoreancestry, bool nodiffdeleted, 
			   bool ignorecontenttype,  const wstring& options, bool bAppend, 
			   const wstring& outputfile, const wstring& errorfile)
{
	bool del = FALSE;
	apr_file_t * outfile;
	apr_file_t * errfile;
	apr_array_header_t *opts;

	SVNPool localpool(pool);

	opts = svn_cstring_split(CUnicodeUtils::StdGetUTF8(options).c_str(), " \t\n\r", TRUE, localpool);

	apr_int32_t flags = APR_WRITE | APR_CREATE | APR_BINARY;
	if (bAppend)
		flags |= APR_APPEND;
	else
		flags |= APR_TRUNCATE;
	Err = svn_io_file_open (&outfile, CUnicodeUtils::StdGetUTF8(outputfile).c_str(),
		flags,
		APR_OS_DEFAULT, localpool);
	if (Err)
		return false;

	wstring workingErrorFile;
	if (errorfile.empty())
	{
		workingErrorFile = CTempFiles::Instance().GetTempFilePath(true);
		del = TRUE;
	}
	else
	{
		workingErrorFile = errorfile;
	}

	Err = svn_io_file_open (&errfile, CUnicodeUtils::StdGetUTF8(workingErrorFile).c_str(),
		APR_WRITE | APR_CREATE | APR_TRUNCATE | APR_BINARY,
		APR_OS_DEFAULT, localpool);
	if (Err)
		return false;

	svn_opt_revision_t rev1;
	rev1.kind = svn_opt_revision_number;
	rev1.value.number = revision1;

	svn_opt_revision_t rev2;
	rev2.kind = svn_opt_revision_number;
	rev2.value.number = revision2;


	Err = svn_client_diff4 (opts,
		svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(url1).c_str(), localpool),
		&rev1,
		svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(url2).c_str(), localpool),
		&rev2,
		svn_depth_infinity,
		ignoreancestry,
		nodiffdeleted,
		ignorecontenttype,
		APR_LOCALE_CHARSET,
		outfile,
		errfile,
		m_pctx,
		localpool);
	if (Err)
	{
		return false;
	}
	if (del)
	{
		svn_io_remove_file (CUnicodeUtils::StdGetUTF8(workingErrorFile).c_str(), localpool);
	}
	return true;
}

wstring SVN::CanonicalizeURL(const wstring& url)
{
	SVNPool localpool(pool);
	return CUnicodeUtils::StdGetUnicode(string(svn_path_canonicalize(CUnicodeUtils::StdGetUTF8(url).c_str(), localpool)));
}