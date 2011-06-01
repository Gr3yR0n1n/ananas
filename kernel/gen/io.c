#include <machine/param.h>
#include <ananas/types.h>
#include <ananas/bio.h>
#include <ananas/device.h>
#include <ananas/lib.h>
#include <ananas/handle.h>
#include <ananas/handle-options.h>
#include <ananas/error.h>
#include <ananas/thread.h>
#include <ananas/pcpu.h>
#include <ananas/stat.h>
#include <ananas/schedule.h>
#include <ananas/syscall.h>
#include <ananas/syscalls.h>
#include <ananas/trace.h>
#include <ananas/vfs.h>
#include <elf.h>

TRACE_SETUP;

errorcode_t
sys_read(thread_t* t, handle_t handle, void* buf, size_t* len)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p, buf=%p, len=%p", t, handle, buf, len);
	errorcode_t err;

	/* Fetch the file handle */
	struct VFS_FILE* file;
	err = syscall_get_file(t, handle, &file);
	ANANAS_ERROR_RETURN(err);

	/* Fetch the size operand */
	size_t size;
	err = syscall_fetch_size(t, len, &size);
	ANANAS_ERROR_RETURN(err);

	/* Attempt to map the buffer write-only */
	void* buffer;
	err = syscall_map_buffer(t, buf, size, THREAD_MAP_WRITE, &buffer);
	ANANAS_ERROR_RETURN(err);

	/* And read data to it */
	err = vfs_read(file, buffer, &size);
	ANANAS_ERROR_RETURN(err);

	/* Finally, inform the user of the length read - the read went OK */
	err = syscall_set_size(t, len, size);
	ANANAS_ERROR_RETURN(err);

	TRACE(SYSCALL, FUNC, "t=%p, success: size=%u", t, size);
	return err;
}

errorcode_t
sys_write(thread_t* t, handle_t handle, const void* buf, size_t* len)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p, buf=%p, len=%p", t, handle, buf, len);
	errorcode_t err;

	/* Fetch the file handle */
	struct VFS_FILE* file;
	err = syscall_get_file(t, handle, &file);
	ANANAS_ERROR_RETURN(err);

	/* Fetch the size operand */
	size_t size;
	err = syscall_fetch_size(t, len, &size);
	ANANAS_ERROR_RETURN(err);

	/* Attempt to map the buffer readonly */
	void* buffer;
	err = syscall_map_buffer(t, buf, size, THREAD_MAP_READ, &buffer);
	ANANAS_ERROR_RETURN(err);

	/* And write data from to it */
	err = vfs_write(file, buffer, &size);
	ANANAS_ERROR_RETURN(err);

	/* Finally, inform the user of the length read - the read went OK */
	err = syscall_set_size(t, len, size);
	ANANAS_ERROR_RETURN(err);

	TRACE(SYSCALL, FUNC, "t=%p, success: size=%u", t, size);
	return err;
}

errorcode_t
sys_open(thread_t* t, struct OPEN_OPTIONS* opts, handle_t* result)
{
	TRACE(SYSCALL, FUNC, "t=%p, opts=%p, result=%p", t, opts, result);
	errorcode_t err;

	/* Obtain open options */
	struct OPEN_OPTIONS open_opts; void* optr;
	err = syscall_map_buffer(t, opts, sizeof(open_opts), THREAD_MAP_READ, &optr);
	ANANAS_ERROR_RETURN(err);
	memcpy(&open_opts, optr, sizeof(open_opts));
	if (open_opts.op_size != sizeof(open_opts))
		return ANANAS_ERROR(BAD_LENGTH);

	/* Fetch the path name */
	const char* userpath;
	err = syscall_map_string(t, open_opts.op_path, &userpath);
	ANANAS_ERROR_RETURN(err);

	/* Obtain a new file handle */
	struct HANDLE* handle;
	err = handle_alloc(HANDLE_TYPE_FILE, t, &handle);
	ANANAS_ERROR_RETURN(err);

	/* XXX we should do something with the mode */

	/* And open the path */
	TRACE(SYSCALL, INFO, "opening userpath '%s'", userpath);
	err = vfs_open(userpath, t->path_handle->data.vfs_file.f_inode, &handle->data.vfs_file);

	/* Finally, hand the handle back if necessary and we're done */
	if (err == ANANAS_ERROR_NONE)
		err = syscall_set_handle(t, result, handle);
	if (err != ANANAS_ERROR_NONE) {
		/* Do not forget to release the handle on error! */
		handle_free(handle);
	} else {
		TRACE(SYSCALL, INFO, "success, handle=%p", handle);
	}
	return err;
}

errorcode_t
sys_destroy(thread_t* t, handle_t handle)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p", t, handle);

	struct HANDLE* h;
	errorcode_t err = syscall_get_handle(t, handle, &h);
	ANANAS_ERROR_RETURN(err);
	err = handle_free(h);
	ANANAS_ERROR_RETURN(err);

	TRACE(SYSCALL, INFO, "t=%p, success", t);
	return err;
}

errorcode_t
sys_clone(thread_t* t, handle_t in, struct CLONE_OPTIONS* opts, handle_t* out)
{
	TRACE(SYSCALL, FUNC, "t=%p, in=%p, opts=%p, out=%p", t, in, opts, out);
	errorcode_t err;

	/* Obtain clone options */
	struct CLONE_OPTIONS clone_opts; void* optr;
	err = syscall_map_buffer(t, opts, sizeof(clone_opts), THREAD_MAP_READ, &optr);
	ANANAS_ERROR_RETURN(err);
	memcpy(&clone_opts, optr, sizeof(clone_opts));
	if (clone_opts.cl_size != sizeof(clone_opts))
		return ANANAS_ERROR(BAD_LENGTH);

	/* Get the handle */
	struct HANDLE* handle;
	err = syscall_get_handle(t, in, &handle);
	ANANAS_ERROR_RETURN(err);

	/* Try to clone it */
	struct HANDLE* dest;
	err = handle_clone(t, handle, &dest);
	ANANAS_ERROR_RETURN(err);

	/*
	 * And hand the cloned handle back - note that the return code will be
	 * overriden for the new child.
	 */
	err = syscall_set_handle(t, out, dest);
	ANANAS_ERROR_RETURN(err);

	TRACE(SYSCALL, FUNC, "t=%p, success, new handle=%p", t, dest);
	return err;
}

errorcode_t
sys_wait(thread_t* t, handle_t handle, handle_event_t* event, handle_event_result_t* result)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p, event=%p, result=%p", t, handle, event, result);
	errorcode_t err;

	/* Fetch the handle */
	struct HANDLE* h;
	err = syscall_get_handle(t, handle, &h);
	ANANAS_ERROR_RETURN(err);

	/* Obtain the event for which to wait */
	handle_event_t wait_event = 0; /* XXX */
	if (event != NULL) {
		handle_event_t* in_event;
		err = syscall_map_buffer(t, event, sizeof(handle_event_t), THREAD_MAP_READ, (void**)&in_event);
		ANANAS_ERROR_RETURN(err);
		wait_event = *in_event;
	}

	/* Wait for the handle */
	handle_event_result_t wait_result;
	err = handle_wait(t, h, &wait_event, &wait_result);
	ANANAS_ERROR_RETURN(err);

	/* If the event type was requested, set it up */
	if (event != NULL) {
		handle_event_t* out_event;
		err = syscall_map_buffer(t, event, sizeof(handle_event_t), THREAD_MAP_WRITE, (void**)&out_event);
		ANANAS_ERROR_RETURN(err);
		*out_event = wait_event;
	}

	/* If the result was requested, set it up */
	if (result != NULL) {
		handle_event_result_t* out_result;
		err = syscall_map_buffer(t, result, sizeof(handle_event_result_t), THREAD_MAP_WRITE, (void*)&out_result);
		ANANAS_ERROR_RETURN(err);
		*out_result = wait_result;
	}

	/* All done */
	TRACE(SYSCALL, FUNC, "t=%p, success, event=0x%x, result=0x%x", t, wait_event, wait_result);
	return ANANAS_ERROR_OK;
}

errorcode_t
sys_summon(thread_t* t, handle_t handle, struct SUMMON_OPTIONS* opts, handle_t* out)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p, opts=%p, out=%p", t, handle, opts, out);
	errorcode_t err;

	/*
	 * XXX This limits summoning to file-based handles.
	 */
	struct VFS_FILE* file;
	err = syscall_get_file(t, handle, &file);
	ANANAS_ERROR_RETURN(err);

	/* Obtain summoning options */
	struct SUMMON_OPTIONS summon_opts; void* so;
	err = syscall_map_buffer(t, opts, sizeof(summon_opts), THREAD_MAP_READ, &so);
	ANANAS_ERROR_RETURN(err);
	memcpy(&summon_opts, so, sizeof(summon_opts));

	/* Create a new thread */
	thread_t* newthread = NULL;
	err = thread_alloc(t, &newthread);
	ANANAS_ERROR_RETURN(err);

	/* XXX should be more general */
	err = elf_load_from_file(newthread, file->f_inode);
	if (err != ANANAS_ERROR_NONE) {
		/* Failure - no option but to kill the thread */
		goto fail;
	}

	/* If arguments are given, handle them */
	if (summon_opts.su_args != NULL) {
		const char* arg;
		err = syscall_map_buffer(t, summon_opts.su_args, summon_opts.su_args_len, THREAD_MAP_READ, (void**)&arg);
		if (err == ANANAS_ERROR_NONE)
			err = thread_set_args(newthread, arg, summon_opts.su_args_len);
		if (err != ANANAS_ERROR_NONE)
			goto fail;
	}

	/* If an environment is given, handle it */
	if (summon_opts.su_env != NULL) {
		const char* env;
		err = syscall_map_buffer(t, summon_opts.su_env, summon_opts.su_env_len, THREAD_MAP_READ, (void**)&env);
		if (err == ANANAS_ERROR_NONE)
			err = thread_set_environment(newthread, env, summon_opts.su_env_len);
		if (err != ANANAS_ERROR_NONE)
			goto fail;
	}

	/* If we need to resume the thread, do so */
	if (summon_opts.su_flags & SUMMON_FLAG_RUNNING)
		thread_resume(newthread);

	/* Finally, hand the handle back if necessary and we're done */
	err = syscall_set_handle(t, out, newthread->thread_handle);
	if (err == ANANAS_ERROR_NONE) {
		TRACE(SYSCALL, INFO, "t=%p, success, thread handle=%p", t, newthread->thread_handle);
		return ANANAS_ERROR_OK;
	}

fail:
	if (newthread != NULL) {
		thread_free(newthread);
		thread_destroy(newthread);
	}
	return err;
}

errorcode_t
sys_create(thread_t* t, struct CREATE_OPTIONS* opts, handle_t* out)
{
	TRACE(SYSCALL, FUNC, "t=%p, opts=%p, out=%p", t, opts, out);
	errorcode_t err;

	/* Obtain arguments */
	struct CREATE_OPTIONS cropts;
	void* opts_ptr;
	err = syscall_map_buffer(t, opts, sizeof(cropts), THREAD_MAP_READ, &opts_ptr);
	ANANAS_ERROR_RETURN(err);
	memcpy(&cropts, opts_ptr, sizeof(cropts));

	if (cropts.cr_size != sizeof(cropts))
		return ANANAS_ERROR(BAD_LENGTH);

	struct HANDLE* outhandle;
	switch(cropts.cr_type) {
		case CREATE_TYPE_FILE: {
			/* Fetch the new path name */
			const char* path;
			err = syscall_map_string(t, cropts.cr_path, &path);
			ANANAS_ERROR_RETURN(err);

			/* Fetch a new handle first; this will likely work */
			err = handle_alloc(HANDLE_TYPE_FILE, t, &outhandle);
			ANANAS_ERROR_RETURN(err);

			/* Attempt to create the new file */
			err = vfs_create(t->path_handle->data.vfs_file.f_inode, &outhandle->data.vfs_file, path, cropts.cr_mode);
			if (err == ANANAS_ERROR_NONE) {
				/* This worked; hand the handle to the thread */
				err = syscall_set_handle(t, out, outhandle);
				if (err != ANANAS_ERROR_NONE) {
					/* Remove the handle (frees the inode as well) */
					handle_free(outhandle);
				} else {
					TRACE(SYSCALL, INFO, "t=%p, success, result handle=%p", t, outhandle);
				}
			} else {
				/* Failure - throw away the handle we created */
				handle_free(outhandle);
			}
			return err;
		}
		case CREATE_TYPE_MEMORY: {
			/* See if the flags mask works */
			if ((cropts.cr_flags & ~(CREATE_MEMORY_FLAG_MASK)) != 0)
				return ANANAS_ERROR(BAD_FLAG);

			/* Create the memory handle */
			err = handle_alloc(HANDLE_TYPE_MEMORY, t, &outhandle);
			ANANAS_ERROR_RETURN(err);

			/* Fetch memory as needed */
			int map_flags = THREAD_MAP_ALLOC;
			if (cropts.cr_flags & CREATE_MEMORY_FLAG_READ)    map_flags |= THREAD_MAP_READ;
			if (cropts.cr_flags & CREATE_MEMORY_FLAG_WRITE)   map_flags |= THREAD_MAP_WRITE;
			if (cropts.cr_flags & CREATE_MEMORY_FLAG_EXECUTE) map_flags |= THREAD_MAP_EXECUTE;
			struct THREAD_MAPPING* tm;
			err = thread_map(t, (addr_t)NULL, cropts.cr_length, map_flags, &tm);
			if (err == ANANAS_ERROR_NONE)
				err = syscall_set_handle(t, out, outhandle);
			if (err != ANANAS_ERROR_NONE) {
				/* Remove the handle to prevent a leak */
				handle_free(outhandle);
				return err;
			}
			outhandle->data.memory.mapping = tm;
			outhandle->data.memory.addr = (void*)tm->tm_virt;
			outhandle->data.memory.length = tm->tm_len;
			TRACE(SYSCALL, INFO, "t=%p, success, result handle=%p", t, outhandle);
			return ANANAS_ERROR_OK;
		}
		default:
			return ANANAS_ERROR(BAD_TYPE);
	}

	/* NOTREACHED */	
}

errorcode_t
sys_unlink(thread_t* t, handle_t handle)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p", t, handle);
	errorcode_t err;

	/* Fetch the handle */
	struct HANDLE* h;
	err = syscall_get_handle(t, handle, &h);
	ANANAS_ERROR_RETURN(err);

	kprintf("sys_unlink(): todo\n");
	return ANANAS_ERROR(BAD_SYSCALL);
}

static errorcode_t
sys_handlectl_file(thread_t* t, handle_t handle, unsigned int op, void* arg, size_t len)
{
	errorcode_t err;

	/* Grab the file handle - we'll always need it as we're doing files */
	struct VFS_FILE* file;
	err = syscall_get_file(t, handle, &file);
	if (err != ANANAS_ERROR_OK)
		goto fail;

	switch(op) {
		case HCTL_FILE_SETCWD: {
			/* Ensure we are dealing with a directory here */
			if (!S_ISDIR(file->f_inode->i_sb.st_mode)) {
				err = ANANAS_ERROR(NOT_A_DIRECTORY);
				goto fail;
			}

			/* XXX We should lock the thread? */

#if 0
			/*
			 * The inode must be owned by the thread already, so we could just hand
			 * it over without dealing with the reference count. However, closing the
			 * handle (as we don't want the thread to mess with it anymore) is the
			 * safest way to continue - so we just continue by referencing the inode
			 * and then freeing the handle (yes, this is a kludge).
			 *
			 * XXX Why isn't this necessary?
			 */
			vfs_ref_inode(file->inode);
#endif

			/* Disown the previous handle; it is no longer of concern */
			handle_free(t->path_handle);

			/* And update the handle */
			t->path_handle = handle;
			TRACE(SYSCALL, INFO, "t=%p, success", t);
			break;
		}
		case HCTL_FILE_SEEK: {
			/* Ensure we understand the whence */
			struct HCTL_SEEK_ARG* se = arg;
			if (arg == NULL) {
				err = ANANAS_ERROR(BAD_ADDRESS);
				goto fail;
			}
			if (len != sizeof(*se)) {
				err = ANANAS_ERROR(BAD_LENGTH);
				goto fail;
			}
			if (se->se_whence != HCTL_SEEK_WHENCE_SET && se->se_whence != HCTL_SEEK_WHENCE_CUR && se->se_whence != HCTL_SEEK_WHENCE_END) {
				err = ANANAS_ERROR(BAD_FLAG);
				goto fail;
			}
			off_t offset;
			err = syscall_fetch_offset(t, se->se_offs, &offset);
			if (err != ANANAS_ERROR_OK)
				return err;
			/* Update the offset */
			switch(se->se_whence) {
				case HCTL_SEEK_WHENCE_SET:
					break;
				case HCTL_SEEK_WHENCE_CUR:
					offset = file->f_offset + offset;
					break;
				case HCTL_SEEK_WHENCE_END:
					offset = file->f_inode->i_sb.st_size - offset;
					break;
			}
			if (offset < 0 || offset >= file->f_inode->i_sb.st_size) {
				err = ANANAS_ERROR(BAD_RANGE);
				goto fail;
			}
			err = syscall_set_offset(t, se->se_offs, offset);
			if (err != ANANAS_ERROR_OK)
				goto fail;
			file->f_offset = offset;
			TRACE(SYSCALL, INFO, "t=%p, success, offset=%u", t, (int)offset);
			break;
		}
		case HCTL_FILE_STAT: {
			/* Ensure we understand the arguments */
			struct HCTL_STAT_ARG* st = arg;
			if (arg == NULL) {
				err = ANANAS_ERROR(BAD_ADDRESS);
				goto fail;
			}
			if (len != sizeof(*st)) {
				err = ANANAS_ERROR(BAD_LENGTH);
				goto fail;
			}
			if (st->st_stat_len != sizeof(struct stat)) {
				err = ANANAS_ERROR(BAD_LENGTH);
				goto fail;
			}

			void* dest;
			err = syscall_map_buffer(t, st->st_stat, sizeof(struct stat), THREAD_MAP_WRITE, &dest);
			if (err != ANANAS_ERROR_OK)
				goto fail;

			if (file->f_inode != NULL) {
				/* Copy the data and we're done */
				memcpy(dest, &file->f_inode->i_sb, sizeof(struct stat));
			} else {
				/* First of all, start by filling with defaults */
				struct stat* st = dest;
				st->st_dev     = (dev_t)(uintptr_t)file->f_device;
				st->st_ino     = (ino_t)0;
				st->st_mode    = 0666;
				st->st_nlink   = 1;
				st->st_uid     = 0;
				st->st_gid     = 0;
				st->st_rdev    = (dev_t)(uintptr_t)file->f_device;
				st->st_size    = 0;
				st->st_atime   = 0;
				st->st_mtime   = 0;
				st->st_ctime   = 0;
				st->st_blksize = BIO_SECTOR_SIZE;
				if (file->f_device->driver->drv_stat != NULL) {
					/* Allow the device driver to update */
					err = file->f_device->driver->drv_stat(file->f_device, st);
				}
			}
			if (err == ANANAS_ERROR_NONE)
				TRACE(SYSCALL, INFO, "t=%p, success", t);
			break;
		}
		default:
			/* What's this? */
			err = ANANAS_ERROR(BAD_SYSCALL);
			break;
	}

fail:
	return err;
}

static errorcode_t
sys_handlectl_memory(thread_t* t, handle_t handle, unsigned int op, void* arg, size_t len)
{
	/* Ensure we are dealing with a memory handle here */
	struct HANDLE* h = handle;
	if (h->type != HANDLE_TYPE_MEMORY)
		return ANANAS_ERROR(BAD_HANDLE);

	switch(op) {
		case HCTL_MEMORY_GET_INFO: {
			/* Ensure the structure length is sane */
			struct HCTL_MEMORY_GET_INFO_ARG* in = arg;
			if (arg == NULL)
				return ANANAS_ERROR(BAD_ADDRESS);
			if (len != sizeof(*in))
				return ANANAS_ERROR(BAD_LENGTH);
			in->in_base = h->data.memory.addr;
			in->in_length = h->data.memory.length;
			TRACE(SYSCALL, INFO, "t=%p, success", t);
			return ANANAS_ERROR_OK;
		}
	}

	/* What's this? */
	return ANANAS_ERROR(BAD_SYSCALL);
}

errorcode_t
sys_handlectl(thread_t* t, handle_t handle, unsigned int op, void* arg, size_t len)
{
	TRACE(SYSCALL, FUNC, "t=%p, handle=%p, op=%u, arg=%p, len=0x%x", t, handle, op, arg, len);
	errorcode_t err;

	/* Fetch the handle */
	struct HANDLE* h;
	err = syscall_get_handle(t, handle, &h);
	ANANAS_ERROR_RETURN(err);

	/* Lock the handle; we don't want it leaving our sight */
	spinlock_lock(&h->spl_handle);

	/* Obtain arguments (note that some calls do not have an argument) */
	void* handlectl_arg = NULL;
	if (arg != NULL) {
		err = syscall_map_buffer(t, arg, len, THREAD_MAP_READ | THREAD_MAP_WRITE, &handlectl_arg);
		if(err != ANANAS_ERROR_OK)
			goto fail;
	}

	if (op >= _HCTL_FILE_FIRST && op <= _HCTL_FILE_LAST)
		err = sys_handlectl_file(t, handle, op, handlectl_arg, len);
	else if (op >= _HCTL_MEMORY_FIRST && op <= _HCTL_MEMORY_LAST)
		err = sys_handlectl_memory(t, handle, op, handlectl_arg, len);
	else
		err = ANANAS_ERROR(BAD_SYSCALL);

fail:
	spinlock_unlock(&h->spl_handle);
	return err;
}

/* vim:set ts=2 sw=2: */
